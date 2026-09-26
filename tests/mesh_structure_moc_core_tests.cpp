#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"
#include <QtEndian>
using namespace live2dstruct;
using Builder = CMocMeshStructureBuilder;
namespace {
Builder::MeshEdit identityMesh(const CSectionTable& table, int index) {
    const auto* mesh = table.art_meshes[index]; Builder::MeshEdit result; result.artMeshId = mesh->ids;
    for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) { result.vertices.append({{vertex}, {1}}); result.oldToNew.append(vertex); }
    for (int i = 0; i < mesh->position_index_sources_counts; ++i) result.indices.append(quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices + i]));
    return result;
}
Builder::MeshEdit removeVertex(const CSectionTable& table, int mesh, int removed) {
    auto edit = identityMesh(table, mesh); edit.vertices.removeAt(removed);
    for (int old = 0; old < edit.oldToNew.size(); ++old) edit.oldToNew[old] = old == removed ? -1 : old - (old > removed);
    QVector<quint16> indices;
    for (int i = 0; i < edit.indices.size(); i += 3) {
        if (edit.indices[i] == removed || edit.indices[i + 1] == removed || edit.indices[i + 2] == removed) continue;
        for (int side = 0; side < 3; ++side) indices.append(edit.oldToNew[edit.indices[i + side]]);
    }
    edit.indices = indices; return edit;
}
QString mappedDifference(csmModel* expected, csmModel* actual, int target, const QVector<int>& newToOld) {
    const int count = csmGetDrawableCount(expected);
    if (csmGetDrawableCount(actual) != count) return "drawable count";
    for (int mesh = 0; mesh < count; ++mesh) {
        const int oldCount = csmGetDrawableVertexCounts(expected)[mesh], newCount = csmGetDrawableVertexCounts(actual)[mesh];
        if (newCount != (mesh == target ? newToOld.size() : oldCount)) return "vertex count";
        for (int vertex = 0; vertex < newCount; ++vertex) {
            const int old = mesh == target ? newToOld[vertex] : vertex;
            const auto& current = csmGetDrawableVertexPositions(actual)[mesh][vertex];
            if (!std::isfinite(current.X) || !std::isfinite(current.Y)) return "nonfinite new point";
            if (old < 0) continue;
            if (raw(csmGetDrawableVertexPositions(expected)[mesh] + old, 1) != raw(&current, 1))
                return QString("position mesh %1 new %2 old %3").arg(mesh).arg(vertex).arg(old);
            if (raw(csmGetDrawableVertexUvs(expected)[mesh] + old, 1) != raw(csmGetDrawableVertexUvs(actual)[mesh] + vertex, 1)) {
                const auto& e = csmGetDrawableVertexUvs(expected)[mesh][old]; const auto& a = csmGetDrawableVertexUvs(actual)[mesh][vertex];
                return QString("uv mesh %1 new %2 old %3: (%4,%5) != (%6,%7)").arg(mesh).arg(vertex).arg(old).arg(e.X).arg(e.Y).arg(a.X).arg(a.Y);
            }
        }
        if (csmGetDrawableOpacities(expected)[mesh] != csmGetDrawableOpacities(actual)[mesh] ||
            raw(csmGetDrawableMultiplyColors(expected) + mesh, 1) != raw(csmGetDrawableMultiplyColors(actual) + mesh, 1) ||
            raw(csmGetDrawableScreenColors(expected) + mesh, 1) != raw(csmGetDrawableScreenColors(actual) + mesh, 1)) return "opacity/colors";
        if (csmGetDrawableDrawOrders(expected)[mesh] != csmGetDrawableDrawOrders(actual)[mesh] ||
            csmGetDrawableRenderOrders(expected)[mesh] != csmGetDrawableRenderOrders(actual)[mesh] ||
            csmGetDrawableConstantFlags(expected)[mesh] != csmGetDrawableConstantFlags(actual)[mesh]) return "scalar definition";
        if (mesh != target && (csmGetDrawableIndexCounts(expected)[mesh] != csmGetDrawableIndexCounts(actual)[mesh] ||
            raw(csmGetDrawableIndices(expected)[mesh], csmGetDrawableIndexCounts(expected)[mesh]) != raw(csmGetDrawableIndices(actual)[mesh], csmGetDrawableIndexCounts(actual)[mesh]))) return "sibling topology";
    }
    return {};
}
int drawable(csmModel* model, const QString& id) {
    for (int index = 0; index < csmGetDrawableCount(model); ++index) if (QString::fromUtf8(csmGetDrawableIds(model)[index]) == id) return index;
    return -1;
}
}
class MocMeshStructureCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void geometryAliasesOnlyBlockTheAffectedPool_data() {
        QTest::addColumn<QString>("mode");
        for (const char* name : {"uv-exact", "uv-partial", "uv-odd", "ebo-exact", "ebo-aligned", "ebo-partial", "positions-exact", "positions-partial"})
            QTest::newRow(name) << QString(name);
    }
    void geometryAliasesOnlyBlockTheAffectedPool() {
        QFETCH(QString, mode); auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto originalReader = CReader::FromBytes(bytes); CSectionTable original(originalReader); QVERIFY(original.IsValid());
        int first = -1, second = -1;
        for (int a = 0; a < original.art_meshes.size() && first < 0; ++a) for (int b = a + 1; b < original.art_meshes.size(); ++b) {
            const auto* x = original.art_meshes[a]; const auto* y = original.art_meshes[b];
            if (x->vertex_counts >= 3 && x->vertex_counts == y->vertex_counts && x->keyform_sources_counts >= y->keyform_sources_counts &&
                x->position_index_sources_counts >= 6 && x->keyform_sources_begin_indices != y->keyform_sources_begin_indices) { first = a; second = b; break; }
        }
        QVERIFY(first >= 0); QVERIFY(second >= 0);
        const auto* x = original.art_meshes[first]; const auto* y = original.art_meshes[second]; const auto* offsets = original.sectionOffsetTable->art_meshes;
        if (mode.startsWith("uv")) {
            const int shift = mode == "uv-partial" ? 2 : mode == "uv-odd" ? 1 : 0;
            qToLittleEndian(qint32(x->uv_sources_begin_indices + shift), bytes.data() + offsets->uv_sources_begin_indices + second * 4);
        } else if (mode.startsWith("ebo")) {
            const int shift = mode == "ebo-aligned" ? 3 : mode == "ebo-partial" ? 1 : 0;
            qToLittleEndian(qint32(x->position_index_sources_begin_indices + shift), bytes.data() + offsets->position_index_sources_begin_indices + second * 4);
            qToLittleEndian(qint32(x->position_index_sources_counts - (shift ? 3 : 0)), bytes.data() + offsets->position_index_sources_counts + second * 4);
        } else {
            const int begin = original.art_mesh_keyforms[x->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
            qToLittleEndian(qint32(begin + (mode.endsWith("partial") ? 2 : 0)), bytes.data() + original.sectionOffsetTable->art_mesh_keyforms->keyform_position_sources_begin_indices + y->keyform_sources_begin_indices * 4);
        }
        const auto immutable = bytes; auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        CoreModel expected; QVERIFY2(expected.load(bytes), "The aliased source must itself be accepted by real Core");
        Builder::Request request; request.meshes.append(identityMesh(table, first)); Builder::Result result; Builder::Impact impact; QString error;
        QVERIFY2(Builder::Analyze(bytes, request, impact, &error), qPrintable(error)); QVERIFY(impact.blockingReasons.isEmpty());
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); QCOMPARE(result.bytes, bytes);
        request.meshes[0].vertices.append({{0,1},{.25,.75}});
        if (mode.startsWith("uv")) {
            QVERIFY2(Builder::Analyze(bytes, request, impact, &error), qPrintable(error)); QVERIFY(!impact.blockingReasons.isEmpty());
            QVERIFY(impact.blockingReasons.join('\n').contains("UV")); result.bytes = "sentinel"; result.state.columns.insert(0x178, "keep");
            QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel")); QCOMPARE(result.state.columns.value(0x178), QByteArray("keep"));
            request.meshes[0] = identityMesh(table, first); request.meshes[0].indices.resize(request.meshes[0].indices.size() - 3);
            QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        } else {
            QVERIFY2(Builder::Analyze(bytes, request, impact, &error), qPrintable(error)); QVERIFY(impact.blockingReasons.isEmpty());
            QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        }
        CoreModel actual; QVERIFY(actual.load(result.bytes)); const int target = drawable(expected.value, x->ids); QVERIFY(target >= 0);
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose); const auto difference = mappedDifference(expected.value, actual.value, target, result.meshes.first().newToOld);
            QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        if (mode.startsWith("ebo")) {
            QCOMPARE(raw(csmGetDrawableIndices(expected.value)[target], csmGetDrawableIndexCounts(expected.value)[target]),
                     raw(csmGetDrawableIndices(actual.value)[target], csmGetDrawableIndexCounts(actual.value)[target]));
            request.meshes[0].indices.resize(request.meshes[0].indices.size() - 3);
            QVERIFY2(Builder::Analyze(bytes, request, impact, &error), qPrintable(error)); QVERIFY(!impact.blockingReasons.isEmpty());
            QVERIFY(impact.blockingReasons.join('\n').contains(QStringLiteral("三角索引"))); const auto accepted = result.bytes;
            QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, accepted);
        }
        QCOMPARE(bytes, immutable); QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void glueLocalPairsFollowTheCorrectMeshSide_data() {
        QTest::addColumn<QString>("path"); QTest::addColumn<bool>("sideB");
        for (const QString& name : {QString("Natori"), QString("Rice")}) for (bool side : {false, true})
            QTest::newRow(qPrintable(name + (side ? "-B" : "-A"))) << QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3" << side;
        const auto extra = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (!extra.isEmpty()) for (bool side : {false, true}) QTest::newRow(side ? "Mao-B" : "Mao-A") << extra << side;
    }
    void glueLocalPairsFollowTheCorrectMeshSide() {
        QFETCH(QString, path); QFETCH(bool, sideB); const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        QVERIFY(!table.glue.isEmpty()); const auto* glue = table.glue.first();
        const int meshIndex = sideB ? glue->art_mesh_indices_b : glue->art_mesh_indices_a;
        const int endpoint = table.glue_info[glue->glue_info_sources_begin_indices + int(sideB)]->position_indices;
        auto edit = identityMesh(table, meshIndex); const int other = endpoint ? 0 : 1; QVERIFY(other < edit.vertices.size());
        std::swap(edit.vertices[endpoint], edit.vertices[other]); std::swap(edit.oldToNew[endpoint], edit.oldToNew[other]);
        for (auto& index : edit.indices) index = edit.oldToNew[index];
        Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); QVERIFY(!result.glues.isEmpty());
        CoreModel expected, actual; QVERIFY(expected.load(bytes)); QVERIFY(actual.load(result.bytes));
        const int target = drawable(expected.value, edit.artMeshId); QVERIFY(target >= 0);
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose); const auto difference = mappedDifference(expected.value, actual.value, target, result.meshes.first().newToOld);
            QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void deletingGlueEndpointsMatchesIndependentPairEdits_data() {
        QTest::addColumn<bool>("retarget"); QTest::addColumn<bool>("singlePair");
        for (bool retarget : {false,true}) for (bool single : {false,true}) QTest::newRow(qPrintable(QString("%1-%2").arg(retarget).arg(single))) << retarget << single;
    }
    void deletingGlueEndpointsMatchesIndependentPairEdits() {
        QFETCH(bool, retarget); QFETCH(bool, singlePair); auto bytes = read(QString(TEST_RESOURCES) + "/Rice/Rice.moc3");
        if (singlePair) {
            auto firstReader = CReader::FromBytes(bytes); CSectionTable first(firstReader); QVERIFY(first.IsValid());
            qToLittleEndian(qint32(2), bytes.data() + first.sectionOffsetTable->glue->glue_info_sources_counts);
        }
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid()); QVERIFY(!table.glue.isEmpty());
        const auto* first = table.glue.first(); const int mesh = first->art_mesh_indices_a;
        const int removed = table.glue_info[first->glue_info_sources_begin_indices]->position_indices;
        auto edit = removeVertex(table, mesh, removed); QVERIFY(!edit.indices.isEmpty());
        const int replacement = removed ? 0 : 1; Builder::Request request; request.meshes.append(edit);
        Builder::Impact impact; QString error; QVERIFY(Builder::Analyze(bytes, request, impact, &error)); QVERIFY(!impact.blockingReasons.isEmpty());
        Builder::Result result; result.bytes = "sentinel"; QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel"));
        request.gluePolicy = retarget ? Builder::GlueEndpointPolicy::Retarget : Builder::GlueEndpointPolicy::DropPair;
        auto oracle = bytes;
        for (int index = 0; index < table.glue.size(); ++index) {
            const auto* glue = table.glue[index]; int written = 0;
            for (int pair = 0; pair < glue->glue_info_sources_counts / 2; ++pair) {
                int vertex[2]; float weight[2]; bool drop = false;
                for (int side = 0; side < 2; ++side) {
                    const auto* info = table.glue_info[glue->glue_info_sources_begin_indices + pair * 2 + side]; vertex[side] = info->position_indices; weight[side] = info->weights;
                    if ((side ? glue->art_mesh_indices_b : glue->art_mesh_indices_a) == mesh && vertex[side] == removed) {
                        if (retarget) {
                            request.glueRetargets.append({index, pair, side ? Builder::GlueRetarget::B : Builder::GlueRetarget::A, edit.oldToNew[replacement]});
                            vertex[side] = replacement;
                        } else drop = true;
                    }
                }
                if (drop) continue;
                for (int side = 0; side < 2; ++side) {
                    const int row = glue->glue_info_sources_begin_indices + written++;
                    qToLittleEndian(quint16(vertex[side]), oracle.data() + table.sectionOffsetTable->glue_info->position_indices + row * 2);
                    quint32 bits; std::memcpy(&bits, &weight[side], 4); qToLittleEndian(bits, oracle.data() + table.sectionOffsetTable->glue_info->weights + row * 4);
                }
            }
            qToLittleEndian(qint32(written), oracle.data() + table.sectionOffsetTable->glue->glue_info_sources_counts + index * 4);
        }
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        if (singlePair && !retarget) QCOMPARE(after.glue.first()->glue_info_sources_counts, 0);
        CoreModel expected, actual; QVERIFY(expected.load(oracle)); QVERIFY(actual.load(result.bytes)); const int target = drawable(expected.value, edit.artMeshId); QVERIFY(target >= 0);
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose); const auto difference = mappedDifference(expected.value, actual.value, target, result.meshes.first().newToOld);
            QVERIFY2(difference.isEmpty(), qPrintable(difference));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void sharedOwnersAndBlendBindingsRemainIndependent_data() {
        QTest::addColumn<QString>("path"); for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void sharedOwnersAndBlendBindingsRemainIndependent() {
        QFETCH(QString,path); auto bytes = read(path); auto originalReader = CReader::FromBytes(bytes); CSectionTable original(originalReader); QVERIFY(original.IsValid());
        int first = -1, second = -1, firstShape = -1, secondShape = -1;
        for (int a = 0; a < original.blend_shapes_art_meshes.size() && first < 0; ++a)
            for (int b = a + 1; b < original.blend_shapes_art_meshes.size(); ++b) {
                const int x = original.blend_shapes_art_meshes[a]->target_indices, y = original.blend_shapes_art_meshes[b]->target_indices;
                if (x != y && original.art_meshes[x]->vertex_counts == original.art_meshes[y]->vertex_counts) { first = x; second = y; firstShape = a; secondShape = b; break; }
            }
        if (!original.blend_shapes_art_meshes.isEmpty()) QVERIFY(firstShape >= 0);
        if (first < 0) for (int a = 0; a < original.art_meshes.size() && first < 0; ++a)
            for (int b = a + 1; b < original.art_meshes.size(); ++b) {
                const auto* x = original.art_meshes[a]; const auto* y = original.art_meshes[b];
                if (x->vertex_counts >= 3 && x->vertex_counts == y->vertex_counts && x->keyform_sources_counts >= y->keyform_sources_counts && x->position_index_sources_counts >= 3) { first = a; second = b; break; }
            }
        QVERIFY(first >= 0); QVERIFY(second >= 0);
        const auto* firstMesh = original.art_meshes[first]; const auto* secondMesh = original.art_meshes[second]; const auto* offsets = original.sectionOffsetTable->art_meshes;
        if (firstMesh->keyform_sources_counts >= secondMesh->keyform_sources_counts)
            qToLittleEndian(qint32(firstMesh->keyform_sources_begin_indices), bytes.data() + offsets->keyform_sources_begin_indices + second * 4);
        if (firstShape >= 0) {
            const auto* shape = original.blend_shapes_art_meshes[firstShape]; const auto* shapeOffsets = original.sectionOffsetTable->blend_shapes_art_meshes;
            qToLittleEndian(qint32(shape->blend_shape_keyform_binding_sources_begin_indices), bytes.data() + shapeOffsets->blend_shape_keyform_binding_sources_begin_indices + secondShape * 4);
            qToLittleEndian(qint32(shape->blend_shape_keyform_binding_sources_counts), bytes.data() + shapeOffsets->blend_shape_keyform_binding_sources_counts + secondShape * 4);
        }
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        CoreModel expected; QVERIFY2(expected.load(bytes), "The shared source must itself be accepted by real Core");
        auto edit = identityMesh(table, first); edit.vertices.append({{0,1},{.25,.75}}); Builder::Request request; request.meshes.append(edit);
        Builder::Result result; QString error; const auto immutable = bytes;
        QVERIFY2(Builder::Build(bytes, request, result, &error),qPrintable(error)); QCOMPARE(bytes, immutable);
        CoreModel actual; QVERIFY(actual.load(result.bytes)); const int target = drawable(expected.value, edit.artMeshId); QVERIFY(target >= 0);
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        if (firstShape >= 0) {
            const auto* oldShape = table.blend_shapes_art_meshes[firstShape]; const auto* newShape = after.blend_shapes_art_meshes[firstShape];
            QCOMPARE(after.blend_shapes_art_meshes[secondShape]->blend_shape_keyform_binding_sources_begin_indices,
                     table.blend_shapes_art_meshes[secondShape]->blend_shape_keyform_binding_sources_begin_indices);
            QVERIFY(newShape->blend_shape_keyform_binding_sources_begin_indices >= table.blend_shape_keyform_bindings.size());
            for (int b = 0; b < oldShape->blend_shape_keyform_binding_sources_counts; ++b) {
                const auto* oldBinding = table.blend_shape_keyform_bindings[oldShape->blend_shape_keyform_binding_sources_begin_indices + b];
                const auto* newBinding = after.blend_shape_keyform_bindings[newShape->blend_shape_keyform_binding_sources_begin_indices + b];
                QCOMPARE(newBinding->parameter_binding_sources_indices, oldBinding->parameter_binding_sources_indices);
                QCOMPARE(newBinding->blend_shape_constraint_index_sources_begin_indices, oldBinding->blend_shape_constraint_index_sources_begin_indices);
                QCOMPARE(newBinding->blend_shape_constraint_index_sources_counts, oldBinding->blend_shape_constraint_index_sources_counts);
                for (int k = 0; k < oldBinding->keyform_sources_blend_shape_counts; ++k) {
                    const auto* old = table.art_mesh_keyforms[oldBinding->keyform_sources_blend_shape_indices + k];
                    const auto* next = after.art_mesh_keyforms[newBinding->keyform_sources_blend_shape_indices + k];
                    for (int axis = 0; axis < 2; ++axis) QCOMPARE(after.keyform_positions->xys[next->keyform_position_sources_begin_indices + firstMesh->vertex_counts * 2 + axis],
                        float(.25 * double(table.keyform_positions->xys[old->keyform_position_sources_begin_indices + axis]) + .75 * double(table.keyform_positions->xys[old->keyform_position_sources_begin_indices + 2 + axis])));
                    QCOMPARE(next->opacities, old->opacities); QCOMPARE(next->draw_orders, old->draw_orders);
                    QCOMPARE(next->multiply_color_sources_index, old->multiply_color_sources_index); QCOMPARE(next->screen_color_sources_index, old->screen_color_sources_index);
                }
            }
            auto broken = bytes;
            const auto* binding = table.blend_shape_keyform_bindings[oldShape->blend_shape_keyform_binding_sources_begin_indices];
            Q_UNUSED(binding);
            qToLittleEndian(qint32(table.art_mesh_keyforms.size() + 1), broken.data() + table.sectionOffsetTable->blend_shape_keyform_bindings->keyform_sources_blend_shape_indices + oldShape->blend_shape_keyform_binding_sources_begin_indices * 4);
            const auto accepted = result.bytes; QVERIFY(!Builder::Build(broken, request, result, &error)); QCOMPARE(result.bytes, accepted);
        }
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose); const auto difference = mappedDifference(expected.value, actual.value, target, result.meshes.first().newToOld);
            QVERIFY2(difference.isEmpty(),qPrintable(difference));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void edgeSplitsPreserveExistingCoreGeometry_data() {
        QTest::addColumn<QString>("path"); for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void edgeSplitsPreserveExistingCoreGeometry() {
        QFETCH(QString,path); const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        int mesh = table.blend_shapes_art_meshes.isEmpty() ? 0 : table.blend_shapes_art_meshes.first()->target_indices;
        auto edit = identityMesh(table, mesh); QVERIFY(edit.indices.size() >= 3);
        const int a = edit.indices[0], b = edit.indices[1], added = edit.vertices.size(); QVERIFY(a != b);
        edit.vertices.append({{a,b},{.5,.5}}); QVector<quint16> triangles; int incident = 0;
        for (int i = 0; i < edit.indices.size(); i += 3) {
            bool split = false;
            for (int side = 0; side < 3; ++side) {
                const int u = edit.indices[i + side], v = edit.indices[i + (side + 1) % 3], w = edit.indices[i + (side + 2) % 3];
                if ((u == a && v == b) || (u == b && v == a)) {
                    triangles << quint16(u) << quint16(added) << quint16(w) << quint16(added) << quint16(v) << quint16(w); split = true; ++incident; break;
                }
            }
            if (!split) for (int side = 0; side < 3; ++side) triangles.append(edit.indices[i + side]);
        }
        QVERIFY(incident > 0); QCOMPARE(triangles.size(), edit.indices.size() + incident * 3); edit.indices = triangles;
        Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error),qPrintable(error)); CoreModel expected, actual; QVERIFY(expected.load(bytes)); QVERIFY(actual.load(result.bytes));
        const int target = drawable(expected.value, edit.artMeshId); QVERIFY(target >= 0);
        const auto sourceIndices = identityMesh(table, mesh).indices;
        bool forward = true, reverse = true;
        const auto* coreSource = csmGetDrawableIndices(expected.value)[target];
        for (int i = 0; i < sourceIndices.size(); ++i) {
            forward = forward && coreSource[i] == sourceIndices[i];
            reverse = reverse && coreSource[i] == sourceIndices[i - i % 3 + 2 - i % 3];
        }
        QVERIFY(forward || reverse);
        QVector<quint16> expectedIndices = edit.indices;
        if (!forward) for (int i = 0; i < expectedIndices.size(); i += 3) std::swap(expectedIndices[i], expectedIndices[i + 2]);
        QCOMPARE(raw(csmGetDrawableIndices(actual.value)[target], csmGetDrawableIndexCounts(actual.value)[target]), raw(expectedIndices.constData(),expectedIndices.size()));
        for (const auto& pose : poses(expected.value)) {
            expected.pose(pose); actual.pose(pose); const auto difference = mappedDifference(expected.value, actual.value, target, result.meshes.first().newToOld);
            QVERIFY2(difference.isEmpty(),qPrintable(difference));
        }
        QVERIFY(expected.intact()); QVERIFY(actual.intact());
    }

    void appendedUnreferencedVertexKeepsOriginalCoreVertices_data() {
        QTest::addColumn<QString>("path"); for (const auto& model : models()) QTest::newRow(qPrintable(model.first)) << model.second;
    }
    void appendedUnreferencedVertexKeepsOriginalCoreVertices() {
        QFETCH(QString,path); const auto bytes = read(path); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        int target = 0; if (!table.blend_shapes_art_meshes.isEmpty()) target = table.blend_shapes_art_meshes.first()->target_indices;
        auto edit = identityMesh(table, target); QVERIFY(edit.vertices.size() >= 2); edit.vertices.append({{0,1},{.25,.75}});
        Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error),qPrintable(error)); CoreModel before, after; QVERIFY(before.load(bytes)); QVERIFY(after.load(result.bytes));
        const int changedMesh = drawable(before.value, edit.artMeshId); QVERIFY(changedMesh >= 0);
        for(const auto& pose : poses(before.value)) {
            before.pose(pose); after.pose(pose);
            QCOMPARE(csmGetDrawableCount(before.value), csmGetDrawableCount(after.value));
            for(int mesh=0;mesh<csmGetDrawableCount(before.value);++mesh) {
                const int count=csmGetDrawableVertexCounts(before.value)[mesh]; QCOMPARE(csmGetDrawableVertexCounts(after.value)[mesh], count+(mesh==changedMesh));
                QCOMPARE(raw(csmGetDrawableVertexPositions(before.value)[mesh],count),raw(csmGetDrawableVertexPositions(after.value)[mesh],count));
                QCOMPARE(csmGetDrawableOpacities(before.value)[mesh],csmGetDrawableOpacities(after.value)[mesh]);
                QCOMPARE(raw(csmGetDrawableMultiplyColors(before.value)+mesh,1),raw(csmGetDrawableMultiplyColors(after.value)+mesh,1));
                QCOMPARE(raw(csmGetDrawableScreenColors(before.value)+mesh,1),raw(csmGetDrawableScreenColors(after.value)+mesh,1));
            }
        }
        QVERIFY(before.intact()); QVERIFY(after.intact());
    }
};
QTEST_GUILESS_MAIN(MocMeshStructureCoreTests)
#include "mesh_structure_moc_core_tests.moc"
