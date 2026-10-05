#include "real_core_model_fixture.h"
#include "live2dstruct/cmocparameterstructurebuilder.h"
#include "live2dstruct/cmocpreviewbuilder.h"
#include "live2dstruct/ckeyformbindinggrid.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "live2dstruct/cblendshaperelations.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <vector>

using namespace live2dstruct;
using Builder = CMocParameterStructureBuilder;

namespace {
Builder::Owner ownerFor(const CSectionTable& table, int type)
{
    CKeyformBindingGrid grid;
    if (!grid.Build(table)) return {};
    const QString preferred = type == 0 ? QStringLiteral("ArtMesh271") : type == 1 ? QStringLiteral("Warp133") : QStringLiteral("Rotation3");
    const auto acceptable = [&](const QString& id) { const auto d = grid.Diagnostic(CKeyformBindingGrid::ObjectType(type), id); return d.isEmpty() || d.contains(QStringLiteral("BlendShape")); };
    Builder::Owner fallback;
    if (!type) {
        for (const auto* mesh : table.art_meshes) if (acceptable(mesh->ids)) {
            if (mesh->ids == preferred) return {Builder::Owner::ArtMesh, mesh->ids};
            if (fallback.id.isEmpty()) fallback = {Builder::Owner::ArtMesh, mesh->ids};
        }
    } else {
        for (const auto* deformer : table.deformers) if (deformer->types == type - 1 && acceptable(deformer->ids)) {
            if (deformer->ids == preferred) return {Builder::Owner::Type(type), deformer->ids};
            if (fallback.id.isEmpty()) fallback = {Builder::Owner::Type(type), deformer->ids};
        }
    }
    return fallback;
}

QSet<int> scopeFor(const CSectionTable& table, const Builder::Owner& owner)
{
    QSet<int> result;
    int deformer = -1;
    for (int index = 0; index < table.deformers.size(); ++index) if (table.deformers[index]->ids == owner.id) deformer = index;
    for (int mesh = 0; mesh < table.art_meshes.size(); ++mesh) {
        if (owner.type == Builder::Owner::ArtMesh) { if (table.art_meshes[mesh]->ids == owner.id) result.insert(mesh); continue; }
        int parent = table.art_meshes[mesh]->parent_deformer_indices;
        for (int depth = 0; parent >= 0 && parent < table.deformers.size() && depth < table.deformers.size(); ++depth) {
            if (parent == deformer) { result.insert(mesh); break; }
            parent = table.deformers[parent]->parent_deformer_indices;
        }
    }
    return result;
}
}

class ParameterStructureCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void createAndDeleteUnbound_data()
    {
        QTest::addColumn<QString>("path");
        for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void createAndDeleteUnbound()
    {
        QFETCH(QString, path);
        const auto original = read(path);
        Builder::CreateRequest request; request.parameter.id = QStringLiteral("ParamStructureCore");
        Builder::Result result, restored; QString error;
        QVERIFY2(Builder::Create(original, request, result, &error), qPrintable(error));
        QVERIFY2(Builder::DeleteUnreferenced(result.bytes, request.parameter.id, restored, &error), qPrintable(error));
        CoreModel baseline, added, removed;
        QVERIFY(baseline.load(original)); QVERIFY(added.load(result.bytes)); QVERIFY(removed.load(restored.bytes));
        QCOMPARE(csmGetParameterCount(added.value), csmGetParameterCount(baseline.value) + 1);
        QCOMPARE(csmGetParameterCount(removed.value), csmGetParameterCount(baseline.value));
        for (const auto& pose : poses(baseline.value)) {
            baseline.pose(pose); removed.pose(pose); auto extended = pose; extended.append(.375f); added.pose(extended);
            QVERIFY(changed(frame(baseline.value), frame(added.value)).isEmpty());
            QVERIFY(changed(frame(baseline.value), frame(removed.value)).isEmpty());
        }
        QVERIFY(baseline.intact() && added.intact() && removed.intact());
    }
    void slowAxisAndEditableCorner_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("type"); QTest::addColumn<int>("keyCount");
        for (const auto& model : models()) for (int type = 0; type < 3; ++type) for (int keys : {2, 3, 4})
            QTest::newRow(qPrintable(model.first + '-' + QString::number(type) + '-' + QString::number(keys))) << model.second << type << keys;
    }
    void slowAxisAndEditableCorner()
    {
        QFETCH(QString, path); QFETCH(int, type); QFETCH(int, keyCount);
        exercise(read(path), path, type, keyCount);
    }
    void sharedSourcesRemainIsolated_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("type");
        for (const auto& model : models()) for (int type = 0; type < 3; ++type)
            QTest::newRow(qPrintable(model.first + '-' + QString::number(type))) << model.second << type;
    }
    void sharedSourcesRemainIsolated()
    {
        QFETCH(QString, path); QFETCH(int, type);
        auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        auto owner = ownerFor(table, type);
        const auto put = [&](int offset, int row, int value) { qToLittleEndian(qint32(value), bytes.data() + offset + row * 4); };
        bool found = false;
        if (!type) {
            for (int first = 0; first < table.art_meshes.size() && !found; ++first) for (int second = first + 1; second < table.art_meshes.size(); ++second) {
                const auto* a = table.art_meshes[first]; const auto* b = table.art_meshes[second];
                if (a->vertex_counts != b->vertex_counts) continue;
                const auto& columns = *table.sectionOffsetTable->art_meshes;
                put(columns.keyform_binding_sources_indices, second, a->keyform_binding_sources_indices);
                put(columns.keyform_sources_begin_indices, second, a->keyform_sources_begin_indices);
                put(columns.keyform_sources_counts, second, a->keyform_sources_counts);
                if (columns.keyform_color_sources_begin_indices) put(columns.keyform_color_sources_begin_indices, second,
                    qFromLittleEndian<qint32>(bytes.constData() + columns.keyform_color_sources_begin_indices + first * 4));
                owner = {Builder::Owner::ArtMesh, a->ids}; found = true; break;
            }
        } else {
            int first = -1;
            for (int index = 0; index < table.deformers.size(); ++index) if (table.deformers[index]->ids == owner.id) first = index;
            QVERIFY(first >= 0);
            for (int second = 0; second < table.deformers.size(); ++second) {
                const auto* candidate = table.deformers[second];
                if (candidate->types != type - 1 || second == first) continue;
                const auto& columns = *table.sectionOffsetTable->deformers;
                put(columns.specific_sources_indices, second, table.deformers[first]->specific_sources_indices);
                put(columns.keyform_binding_sources_indices, second, table.deformers[first]->keyform_binding_sources_indices);
                found = true; break;
            }
        }
        QVERIFY(found);
        if (type) {
            CoreModel acceptedSource;
            QVERIFY(acceptedSource.load(bytes));
            Builder::CreateRequest request;
            request.parameter.id = QStringLiteral("ParamSharedSpecific");
            request.bindings.append({owner, {-1, 0, 1}});
            Builder::Result retained; retained.bytes = "sentinel";
            QString error;
            QVERIFY(!Builder::Create(bytes, request, retained, &error));
            QVERIFY(error.contains(QStringLiteral("共享专用")));
            QCOMPARE(retained.bytes, QByteArray("sentinel"));
            request.bindings.clear();
            Builder::Result unbound;
            QVERIFY(Builder::Create(bytes, request, unbound, &error));
            QVERIFY(!Builder::BindExistingParameter(unbound.bytes, request.parameter.id, {{owner, {-1, 0, 1}}}, retained, &error));
            QCOMPARE(retained.bytes, QByteArray("sentinel"));
            CModelDocument document;
            QVERIFY(document.LoadBytes(bytes, path, &error));
            CMocPreviewBuilder preview; QVERIFY(preview.Initialize(bytes, &error));
            CMocPreviewBuilder::Overrides edits;
            if (type == 1) { auto values = document.DeformerWarpKeyforms(owner.id); values[0].positions[0] += .03f; edits.warpKeyforms.insert(owner.id, values); }
            else { auto values = document.DeformerRotationKeyforms(owner.id); values[0].originX += .03f; edits.rotationKeyforms.insert(owner.id, values); }
            QByteArray output("sentinel");
            QVERIFY(!preview.BuildOverrides(edits, output, &error));
            QVERIFY(error.contains(QStringLiteral("共享专用")));
            QCOMPARE(output, QByteArray("sentinel"));
            QVERIFY(acceptedSource.intact());
            return;
        }
        exercise(bytes, path, type, 3, owner);
    }

    void acceptedSharedSpecificCanHaveCoupledCoreSemantics()
    {
        const QString path = QString(TEST_RESOURCES) + "/Natori/Natori.moc3";
        auto shared = read(path); auto reader = CReader::FromBytes(shared); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int first = -1, second = -1;
        for (int index = 0; index < table.deformers.size(); ++index) {
            if (table.deformers[index]->ids == QStringLiteral("Rotation3")) first = index;
            if (table.deformers[index]->ids == QStringLiteral("Rotation17")) second = index;
        }
        QVERIFY(first >= 0 && second >= 0);
        const int source = table.deformers[first]->specific_sources_indices;
        const int orphan = table.deformers[second]->specific_sources_indices;
        const auto& columns = *table.sectionOffsetTable->deformers;
        qToLittleEndian(qint32(source), shared.data() + columns.specific_sources_indices + second * 4);
        qToLittleEndian(qint32(table.deformers[first]->keyform_binding_sources_indices), shared.data() + columns.keyform_binding_sources_indices + second * 4);
        QByteArray independent = shared;
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            if (QByteArray(span.name).startsWith("rotation_deformers.") && span.offset)
                independent.replace(span.offset + orphan * 4, 4, shared.mid(span.offset + source * 4, 4));
        qToLittleEndian(qint32(orphan), independent.data() + columns.specific_sources_indices + first * 4);
        CoreModel baseline, detached;
        QVERIFY(baseline.load(shared)); QVERIFY(detached.load(independent));
        bool different = false;
        for (const auto& pose : poses(baseline.value)) {
            baseline.pose(pose); detached.pose(pose);
            different = different || !changed(frame(baseline.value), frame(detached.value)).isEmpty();
        }
        QVERIFY(different);
        QVERIFY(baseline.intact() && detached.intact());
    }

    void independentSpecificSourcesCanShareKeyforms_data()
    {
        QTest::addColumn<QString>("path"); QTest::addColumn<int>("type");
        for (const auto& model : models()) for (int type : {1, 2})
            QTest::newRow(qPrintable(model.first + '-' + QString::number(type))) << model.second << type;
    }
    void independentSpecificSourcesCanShareKeyforms()
    {
        QFETCH(QString, path); QFETCH(int, type);
        auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        Builder::Owner owner;
        int first = -1, second = -1;
        CKeyformBindingGrid grid; QVERIFY(grid.Build(table));
        for (int a = 0; a < table.deformers.size() && first < 0; ++a) {
            const auto* target = table.deformers[a];
            if (target->types != type - 1) continue;
            const Builder::Owner selected{Builder::Owner::Type(type), target->ids};
            if (scopeFor(table, selected).isEmpty()) continue;
            const auto diagnostic = grid.Diagnostic(CKeyformBindingGrid::ObjectType(type), target->ids);
            if (!diagnostic.isEmpty() && !diagnostic.contains(QStringLiteral("BlendShape"))) continue;
            for (int b = 0; b < table.deformers.size(); ++b) {
                const auto* sibling = table.deformers[b];
                if (sibling->types != type - 1 || sibling->specific_sources_indices == target->specific_sources_indices ||
                    sibling->parent_deformer_indices != target->parent_deformer_indices) continue;
                first = a; second = b; owner = selected; break;
            }
        }
        QVERIFY(first >= 0 && second >= 0);
        const auto targetScope = scopeFor(table, owner);
        if (scopeFor(table, {Builder::Owner::Type(type), table.deformers[second]->ids}).isEmpty() && targetScope.size() > 1) {
            const int mesh = *std::min_element(targetScope.cbegin(), targetScope.cend());
            qToLittleEndian(qint32(second), bytes.data() + table.sectionOffsetTable->art_meshes->parent_deformer_indices + mesh * 4);
        }
        const int a = table.deformers[first]->specific_sources_indices, b = table.deformers[second]->specific_sources_indices;
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            if (span.countIndex == (type == 1 ? 2 : 3) && span.offset)
                bytes.replace(span.offset + b * span.width, span.width, bytes.mid(span.offset + a * span.width, span.width));
        qToLittleEndian(qint32(table.deformers[first]->keyform_binding_sources_indices),
                        bytes.data() + table.sectionOffsetTable->deformers->keyform_binding_sources_indices + second * 4);
        exercise(bytes, path, type, 3, owner);
    }

    void expandedBlendShapeTables_data() { createAndDeleteUnbound_data(); }
    void expandedBlendShapeTables()
    {
        QFETCH(QString, path);
        const auto original = read(path); auto reader = CReader::FromBytes(original); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto spans = CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info);
        if (table.sectionOffsetTable->mocVersion < 5) { QCOMPARE(spans.size(), 135); return; }
        QCOMPARE(spans.size(), CSectionValidation::KnownOffsetCount(table.sectionOffsetTable->mocVersion) - 2);
        if (table.count_info->blend_shapes_parts > 0) QVERIFY(!table.blend_shapes_parts.empty());
        if (table.count_info->blend_shapes_rotation_deformers > 0) QVERIFY(!table.blend_shapes_rotation_deformers.empty());
        Builder::ColumnState state;
        for (int countIndex : {32, 33}) {
            for (const auto& span : spans) if (span.countIndex == countIndex) {
                const auto first = original.mid(span.offset, 4);
                QByteArray bytes;
                for (int row = 0; row < 17; ++row) bytes.append(first);
                bytes.append(original.mid(span.offset, span.count * span.width));
                state.columns.insert(span.slot, bytes);
                state.counts.insert(countIndex, span.count + 17);
            }
        }
        qToLittleEndian(qint32(0), state.columns[0x27c].data());
        Builder::ColumnState permuted = state;
        for (auto& column : permuted.columns) column = column.right(4) + column.left(column.size() - 4);
        QByteArray expanded; QString error;
        QVERIFY2(Builder::BuildColumnState(original, state, expanded, &error), qPrintable(error));
        QByteArray reordered;
        QVERIFY2(Builder::BuildColumnState(original, permuted, reordered, &error), qPrintable(error));
        CoreModel baseline, candidate, oracle; QVERIFY(baseline.load(original)); QVERIFY(candidate.load(expanded)); QVERIFY(oracle.load(reordered));
        bool effectObserved = false;
        for (const auto& pose : poses(baseline.value)) {
            baseline.pose(pose); candidate.pose(pose); oracle.pose(pose);
            const auto a = frame(oracle.value), b = frame(candidate.value);
            QCOMPARE(a.definition, b.definition);
            QCOMPARE(a.values, b.values);
            for (const auto& values : b.values) for (float value : values) QVERIFY(std::isfinite(value));
            effectObserved = effectObserved || !changed(frame(baseline.value), b).isEmpty();
        }
        QVERIFY(effectObserved);
        Builder::ColumnState captured;
        QVERIFY(Builder::CaptureColumnState(original, expanded, captured, &error));
        QCOMPARE(captured.counts, state.counts);
        QCOMPARE(captured.columns, state.columns);
        QVERIFY(baseline.intact() && candidate.intact() && oracle.intact());
    }

    void blendShapeRelationsMatchNativeRows_data() { createAndDeleteUnbound_data(); }
    void blendShapeRelationsMatchNativeRows()
    {
        QFETCH(QString, path);
        auto reader = CReader::FromBytes(read(path)); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        CBlendShapeRelations relations;
        QString error;
        QVERIFY2(relations.Build(table, &error), qPrintable(error));
        QHash<int, QSet<QPair<int, QString>>> expected;
        for (const auto& group : {qMakePair(CBlendShapeRelations::Part, &table.blend_shapes_parts),
                                 qMakePair(CBlendShapeRelations::Rotation, &table.blend_shapes_rotation_deformers)}) {
            for (const auto* shape : *group.second) {
                QSet<QString> targets;
                if (group.first == CBlendShapeRelations::Part) {
                    QVERIFY(shape->target_indices >= 0 && shape->target_indices < table.parts.size());
                    targets.insert(table.parts[shape->target_indices]->ids);
                } else for (const auto* deformer : table.deformers)
                    if (deformer->types == 1 && deformer->specific_sources_indices == shape->target_indices) targets.insert(deformer->ids);
                QVERIFY(!targets.isEmpty());
                for (int offset = 0; offset < shape->blend_shape_keyform_binding_sources_counts; ++offset) {
                    const int bindingIndex = shape->blend_shape_keyform_binding_sources_begin_indices + offset;
                    QVERIFY(bindingIndex >= 0 && bindingIndex < table.blend_shape_keyform_bindings.size());
                    const auto* binding = table.blend_shape_keyform_bindings[bindingIndex];
                    int owner = -1;
                    for (int parameter = 0; parameter < table.parameters.size(); ++parameter) {
                        const auto* p = table.parameters[parameter];
                        if (binding->parameter_binding_sources_indices >= p->blend_shape_parameter_binding_sources_begin_indices &&
                            binding->parameter_binding_sources_indices < qint64(p->blend_shape_parameter_binding_sources_begin_indices) + p->blend_shape_parameter_binding_sources_counts) {
                            QCOMPARE(owner, -1); owner = parameter;
                        }
                    }
                    QVERIFY(owner >= 0);
                    for (const auto& target : targets) expected[owner].insert(qMakePair(int(group.first), target));
                }
            }
        }
        for (int parameter = 0; parameter < table.parameters.size(); ++parameter) {
            QSet<QPair<int, QString>> actual;
            for (const auto& target : relations.TargetsForParameter(table.parameters[parameter]->ids))
                if (target.type == CBlendShapeRelations::Part || target.type == CBlendShapeRelations::Rotation)
                    actual.insert(qMakePair(int(target.type), target.id));
            QCOMPARE(actual, expected.value(parameter));
        }
    }

private:
    void exercise(const QByteArray& original, const QString& path, int type, int keyCount,
                  const Builder::Owner& requested = {})
    {
        auto reader = CReader::FromBytes(original); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const auto owner = requested.id.isEmpty() ? ownerFor(table, type) : requested; QVERIFY(!owner.id.isEmpty());
        const auto scope = scopeFor(table, owner); QVERIFY(!scope.isEmpty());
        const QVector<float> keys = keyCount == 2 ? QVector<float>{-1, 1} : keyCount == 3 ? QVector<float>{-1, 0, 1} : QVector<float>{-1, -.4f, .2f, 1};
        Builder::CreateRequest request; request.parameter.id = QStringLiteral("ParamStructureCore"); request.bindings.append({owner, keys});
        Builder::Result result; QString error;
        QVERIFY2(Builder::Create(original, request, result, &error), qPrintable(error));
        CoreModel baseline, bound; QVERIFY(baseline.load(original)); QVERIFY(bound.load(result.bytes));
        const int parameterCount = csmGetParameterCount(baseline.value);
        QCOMPARE(csmGetParameterCount(bound.value), parameterCount + 1);
        for (int index = 0; index < parameterCount; ++index) {
            QCOMPARE(QByteArray(csmGetParameterIds(bound.value)[index]), QByteArray(csmGetParameterIds(baseline.value)[index]));
            QCOMPARE(csmGetParameterMinimumValues(bound.value)[index], csmGetParameterMinimumValues(baseline.value)[index]);
            QCOMPARE(csmGetParameterMaximumValues(bound.value)[index], csmGetParameterMaximumValues(baseline.value)[index]);
            QCOMPARE(csmGetParameterDefaultValues(bound.value)[index], csmGetParameterDefaultValues(baseline.value)[index]);
        }
        int poseIndex = 0;
        for (const auto& pose : poses(baseline.value)) {
            auto samples = keys;
            samples += QVector<float>{-.5f, 0, .375f};
            for (float key : samples) {
                baseline.pose(pose); const auto expected = frame(baseline.value);
                auto extended = pose; extended.append(key); bound.pose(extended);
                const auto actual = frame(bound.value);
                const bool exactCorner = keys.contains(key);
                QVERIFY2(exactCorner ? expected.definition == actual.definition && expected.values == actual.values : changed(expected, actual, scope).isEmpty(),
                    qPrintable(QStringLiteral("%1 pose %2 new %3: %4").arg(owner.id).arg(poseIndex).arg(key).arg(difference(expected, actual, scope))));
            }
            ++poseIndex;
        }
        CModelDocument document; QVERIFY2(document.LoadBytes(result.bytes, path, &error), qPrintable(error));
        CMocPreviewBuilder::Overrides overrides;
        const int local = result.keyforms.first().oldKeyformCount * (keyCount - 1);
        if (!type) {
            for (const auto& mesh : document.Drawables()) if (mesh.id == owner.id) {
                QVector<QVector<float>> positions;
                for (const auto& keyform : mesh.keyforms) positions.append(keyform.positions);
                for (int vertex = 0; vertex < positions[local].size(); vertex += 2) positions[local][vertex] += .03f;
                overrides.artMeshPositions.insert(owner.id, positions);
            }
        } else {
            for (const auto& deformer : document.Deformers()) if (deformer.id == owner.id) {
                if (type == 1) {
                    auto keyforms = deformer.warpKeyforms;
                    for (int vertex = 0; vertex < keyforms[local].positions.size(); vertex += 2) keyforms[local].positions[vertex] += .03f;
                    overrides.warpKeyforms.insert(owner.id, keyforms);
                } else { auto keyforms = deformer.rotationKeyforms; keyforms[local].originX += .03f; overrides.rotationKeyforms.insert(owner.id, keyforms); }
            }
        }
        CMocPreviewBuilder preview; QByteArray edited;
        QVERIFY2(preview.Initialize(result.bytes, &error), qPrintable(error));
        QVERIFY2(preview.BuildOverrides(overrides, edited, &error), qPrintable(error));
        CoreModel modified, unchanged; QVERIFY(modified.load(edited)); QVERIFY(unchanged.load(result.bytes));
        CKeyformBindingGrid grid; QVERIFY(grid.Build(table));
        const auto axes = grid.Axes(CKeyformBindingGrid::ObjectType(type), owner.id);
        bool moved = false;
        for (auto corner : poses(baseline.value)) {
            for (const auto& axis : axes) for (int index = 0; index < parameterCount; ++index)
                if (QString::fromUtf8(csmGetParameterIds(baseline.value)[index]) == axis.parameterId) corner[index] = axis.keys.first();
            corner.append(keys.last()); unchanged.pose(corner); modified.pose(corner);
            const auto difference = changed(frame(unchanged.value), frame(modified.value));
            QVERIFY(!difference.contains(-1));
            for (int mesh : difference) QVERIFY2(scope.contains(mesh), qPrintable(QStringLiteral("Changed outside target: %1").arg(mesh)));
            moved = moved || !difference.isEmpty();
            for (int key = 0; key < keys.size() - 1; ++key) {
                corner.last() = keys[key]; unchanged.pose(corner); modified.pose(corner);
                QVERIFY(changed(frame(unchanged.value), frame(modified.value)).isEmpty());
            }
            if (moved) break;
        }
        QVERIFY2(moved, qPrintable(owner.id));
        QVERIFY(baseline.intact() && bound.intact() && modified.intact() && unchanged.intact());
    }
};

QTEST_APPLESS_MAIN(ParameterStructureCoreTests)
#include "parameter_structure_core_tests.moc"
