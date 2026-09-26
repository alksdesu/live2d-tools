#include "live2dstruct/cmesheditplanner.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"

using namespace live2dstruct;
using Planner = CMeshEditPlanner;
using Builder = CMocMeshStructureBuilder;
namespace {
Planner::Input meshInput(const CSectionTable& table, int index)
{
    Planner::Input input; const auto* mesh = table.art_meshes[index]; input.artMeshId = mesh->ids;
    for (int i = 0; i < mesh->vertex_counts * 2; ++i) input.positions.append(table.uvs->uvs[mesh->uv_sources_begin_indices + i]);
    for (int i = 0; i < mesh->position_index_sources_counts; ++i) input.indices.append(quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices + i]));
    return input;
}
QPointF centroid(const Planner::Input& input)
{
    double x = 0, y = 0;
    for (int i = 0; i < 3; ++i) { const int v = input.indices[i]; x += double(input.positions[v * 2]) / 3; y += double(input.positions[v * 2 + 1]) / 3; }
    return {x, y};
}
int drawableIndex(csmModel* model, const QString& id)
{
    for (int i = 0; i < csmGetDrawableCount(model); ++i) if (QString::fromUtf8(csmGetDrawableIds(model)[i]) == id) return i;
    return -1;
}
QString unchangedVertices(csmModel* oldModel, csmModel* newModel, int target, const QVector<int>& oldToNew)
{
    if (csmGetDrawableCount(oldModel) != csmGetDrawableCount(newModel)) return "drawable count";
    for (int mesh = 0; mesh < csmGetDrawableCount(oldModel); ++mesh) {
        const int oldCount = csmGetDrawableVertexCounts(oldModel)[mesh], newCount = csmGetDrawableVertexCounts(newModel)[mesh];
        if (mesh != target && oldCount != newCount) return "sibling vertex count";
        for (int old = 0; old < oldCount; ++old) {
            const int next = mesh == target ? oldToNew[old] : old;
            if (next < 0) continue;
            if (next >= newCount) return "mapped vertex count";
            if (raw(csmGetDrawableVertexPositions(oldModel)[mesh] + old, 1) != raw(csmGetDrawableVertexPositions(newModel)[mesh] + next, 1)) return "surviving positions";
            if (raw(csmGetDrawableVertexUvs(oldModel)[mesh] + old, 1) != raw(csmGetDrawableVertexUvs(newModel)[mesh] + next, 1)) return "surviving UV";
        }
        for (int vertex = 0; vertex < newCount; ++vertex) {
            const auto p = csmGetDrawableVertexPositions(newModel)[mesh][vertex];
            if (!std::isfinite(p.X) || !std::isfinite(p.Y)) return "nonfinite output";
        }
        if (csmGetDrawableOpacities(oldModel)[mesh] != csmGetDrawableOpacities(newModel)[mesh] ||
            csmGetDrawableDrawOrders(oldModel)[mesh] != csmGetDrawableDrawOrders(newModel)[mesh] ||
            csmGetDrawableRenderOrders(oldModel)[mesh] != csmGetDrawableRenderOrders(newModel)[mesh] ||
            raw(csmGetDrawableMultiplyColors(oldModel) + mesh, 1) != raw(csmGetDrawableMultiplyColors(newModel) + mesh, 1) ||
            raw(csmGetDrawableScreenColors(oldModel) + mesh, 1) != raw(csmGetDrawableScreenColors(newModel) + mesh, 1)) return "opacity/order/colors";
        if (mesh != target && raw(csmGetDrawableIndices(oldModel)[mesh], csmGetDrawableIndexCounts(oldModel)[mesh]) != raw(csmGetDrawableIndices(newModel)[mesh], csmGetDrawableIndexCounts(newModel)[mesh])) return "sibling triangles";
    }
    return {};
}
}

class MeshEditPlannerCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void plannedTopologyCompilesIntoCoreAndSurvivesHistory_data()
    {
        QTest::addColumn<QString>("name");
        for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString(name);
        const QString extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (!extra.isEmpty()) QTest::newRow("Mao") << extra;
    }
    void plannedTopologyCompilesIntoCoreAndSurvivesHistory()
    {
        QFETCH(QString, name); const QString path = name.endsWith(".moc3") ? name : QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3";
        const QByteArray source = read(path); QVERIFY(!source.isEmpty()); const QByteArray immutable = source;
        auto reader = CReader::FromBytes(source); CSectionTable original(reader); QVERIFY(original.IsValid());
        QSet<int> blendTargets;
        for (const auto* shape : original.blend_shapes_art_meshes) if (shape->blend_shape_keyform_binding_sources_counts > 0) blendTargets.insert(shape->target_indices);
        Planner::Input input; Planner::Result plan; Planner::Diagnostic diagnostic; int target = -1;
        for (int i = 0; i < original.art_meshes.size(); ++i) {
            if (!blendTargets.isEmpty() && !blendTargets.contains(i)) continue;
            auto candidate = meshInput(original, i); if (candidate.indices.size() < 3) continue;
            if (Planner::Insert(candidate, centroid(candidate), 0, plan, &diagnostic) && !plan.insertedVertices.isEmpty()) { target = i; input = std::move(candidate); break; }
        }
        QVERIFY2(target >= 0, qPrintable("No valid source mesh: " + diagnostic.message));
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source, path, &error), qPrintable(error));
        Builder::Request request; request.meshes = {plan.edit}; CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(command));
        QByteArray inserted; QVERIFY2(document.BuildPreviewMoc(inserted, &error), qPrintable(error));
        CoreModel baseline, modified; QVERIFY(baseline.load(source)); QVERIFY(modified.load(inserted)); const int drawable = drawableIndex(baseline.value, input.artMeshId); QVERIFY(drawable >= 0);
        QCOMPARE(csmGetDrawableVertexCounts(modified.value)[drawable], original.art_meshes[target]->vertex_counts + 1);
        for (const auto& pose : poses(baseline.value)) {
            baseline.pose(pose); modified.pose(pose);
            const auto difference = unchangedVertices(baseline.value, modified.value, drawable, plan.edit.oldToNew); QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        auto updatedReader = CReader::FromBytes(inserted); CSectionTable updated(updatedReader); QVERIFY(updated.IsValid());
        const auto* before = original.art_meshes[target]; const auto* after = updated.art_meshes[target]; const auto& weights = plan.edit.vertices.last();
        QCOMPARE(before->keyform_sources_counts, after->keyform_sources_counts);
        for (int form = 0; form < before->keyform_sources_counts; ++form) {
            const int oldStart = original.art_mesh_keyforms[before->keyform_sources_begin_indices + form]->keyform_position_sources_begin_indices;
            const int newStart = updated.art_mesh_keyforms[after->keyform_sources_begin_indices + form]->keyform_position_sources_begin_indices;
            for (int axis = 0; axis < 2; ++axis) {
                double expected = 0, sum = 0; for (double w : weights.weights) sum += w;
                for (int i = 0; i < weights.indices.size(); ++i) expected += double(original.keyform_positions->xys[oldStart + weights.indices[i] * 2 + axis]) * (weights.weights[i] / sum);
                QCOMPARE(updated.keyform_positions->xys[newStart + before->vertex_counts * 2 + axis], float(expected));
            }
        }
        int checkedBlendForms = 0;
        for (int shapeIndex = 0; shapeIndex < original.blend_shapes_art_meshes.size(); ++shapeIndex) {
            const auto* shape = original.blend_shapes_art_meshes[shapeIndex]; if (shape->target_indices != target) continue;
            const auto* nextShape = updated.blend_shapes_art_meshes[shapeIndex];
            QCOMPARE(nextShape->blend_shape_keyform_binding_sources_counts, shape->blend_shape_keyform_binding_sources_counts);
            for (int binding = 0; binding < shape->blend_shape_keyform_binding_sources_counts; ++binding) {
                const auto* oldBinding = original.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices + binding];
                const auto* newBinding = updated.blend_shape_keyform_bindings[nextShape->blend_shape_keyform_binding_sources_begin_indices + binding];
                QCOMPARE(newBinding->keyform_sources_blend_shape_counts, oldBinding->keyform_sources_blend_shape_counts);
                for (int form = 0; form < oldBinding->keyform_sources_blend_shape_counts; ++form) {
                    ++checkedBlendForms;
                    const int oldStart = original.art_mesh_keyforms[oldBinding->keyform_sources_blend_shape_indices + form]->keyform_position_sources_begin_indices;
                    const int newStart = updated.art_mesh_keyforms[newBinding->keyform_sources_blend_shape_indices + form]->keyform_position_sources_begin_indices;
                    for (int axis = 0; axis < 2; ++axis) {
                        double expected = 0, sum = 0; for (double w : weights.weights) sum += w;
                        for (int i = 0; i < weights.indices.size(); ++i) expected += double(original.keyform_positions->xys[oldStart + weights.indices[i] * 2 + axis]) * (weights.weights[i] / sum);
                        QCOMPARE(updated.keyform_positions->xys[newStart + before->vertex_counts * 2 + axis], float(expected));
                    }
                }
            }
        }
        if (!blendTargets.isEmpty()) QVERIFY(checkedBlendForms > 0);
        CModelSession session; session.version = 5; session.meshStructure = true; session.sourceMoc = path; session.sourceDigest = document.SourceDigest(); session.structure = document.ParameterStructure();
        CModelSession decoded; QVERIFY2(CModelSession::FromJson(session.ToJson(), &decoded, &error), qPrintable(error));
        CModelDocument restored; QVERIFY2(decoded.RestoreDocument(document, restored, &error), qPrintable(error)); QByteArray restoredBytes;
        QVERIFY(restored.BuildPreviewMoc(restoredBytes, &error)); QCOMPARE(restoredBytes, inserted);
        QVERIFY(document.UndoEdit()); QByteArray undone; QVERIFY(document.BuildPreviewMoc(undone, &error)); QCOMPARE(undone, source);
        QVERIFY(document.RedoEdit()); QByteArray redone; QVERIFY(document.BuildPreviewMoc(redone, &error)); QCOMPARE(redone, inserted);
        const auto insertedInput = meshInput(updated, target); Planner::Result removal;
        QVERIFY2(Planner::Delete(insertedInput, plan.insertedVertices, Planner::BoundaryPolicy::PreserveBoundary, removal, &diagnostic), qPrintable(diagnostic.message));
        request.meshes = {removal.edit}; QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(command));
        QByteArray removed; QVERIFY(document.BuildPreviewMoc(removed, &error)); CoreModel contracted; QVERIFY(contracted.load(removed));
        QVector<int> identity; for (int i = 0; i < before->vertex_counts; ++i) identity.append(i);
        for (const auto& pose : poses(baseline.value)) {
            baseline.pose(pose); contracted.pose(pose);
            const auto difference = unchangedVertices(baseline.value, contracted.value, drawable, identity); QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        QCOMPARE(document.SourceBytes(), immutable); QCOMPARE(source, immutable); QVERIFY(baseline.intact()); QVERIFY(modified.intact()); QVERIFY(contracted.intact());
    }
};
QTEST_GUILESS_MAIN(MeshEditPlannerCoreTests)
#include "mesh_edit_planner_core_tests.moc"
