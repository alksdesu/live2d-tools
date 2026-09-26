#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include <QtTest>
#include <QFile>
#include <QtEndian>
#include <cmath>
#include <limits>

using namespace live2dstruct;
using Builder = CMocMeshStructureBuilder;
namespace {
QByteArray source(const QString& name) {
    QFile file(QString(TEST_RESOURCES) + '/' + name + '/' + name + ".moc3");
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}
Builder::MeshEdit identity(const CSectionTable& table, int index) {
    const auto* mesh = table.art_meshes[index]; Builder::MeshEdit result; result.artMeshId = mesh->ids;
    for (int vertex = 0; vertex < mesh->vertex_counts; ++vertex) { result.vertices.append({{vertex}, {1}}); result.oldToNew.append(vertex); }
    for (int i = 0; i < mesh->position_index_sources_counts; ++i) result.indices.append(quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices + i]));
    return result;
}
}
class MocMeshStructureTests : public QObject
{
    Q_OBJECT
private slots:
    void identityPreservesEveryByte_data() {
        QTest::addColumn<QString>("name"); for (const char* name : {"Mark", "Natori", "Rice", "Wanko"}) QTest::newRow(name) << QString(name);
    }
    void identityPreservesEveryByte() {
        QFETCH(QString, name); const auto bytes = source(name); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, {}, result, &error), qPrintable(error)); QCOMPARE(result.bytes, bytes); QVERIFY(result.state.columns.isEmpty());
        Builder::Request request; request.meshes.append(identity(table, 0));
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); QCOMPARE(result.bytes, bytes); QVERIFY(result.state.columns.isEmpty());
    }
    void invalidRequestsPreserveInputAndBothResults_data() {
        QTest::addColumn<int>("mutation"); for (int i = 0; i < 19; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidRequestsPreserveInputAndBothResults() {
        QFETCH(int, mutation); const auto bytes = source("Mark"); const auto original = bytes; auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        Builder::Request request; auto edit = identity(table, 0); edit.vertices.append({{0,1},{.25,.75}}); request.meshes.append(edit);
        auto& item = request.meshes[0];
        switch (mutation) {
        case 0: item.artMeshId = "missing"; break;
        case 1: request.meshes.append(edit); break;
        case 2: item.vertices.clear(); break;
        case 3: item.vertices.last().indices.clear(); break;
        case 4: item.vertices.last().weights = {std::numeric_limits<double>::quiet_NaN(), 1}; break;
        case 5: item.vertices.last().weights = {-.25, 1.25}; break;
        case 6: item.vertices.last().weights = {.25, .7}; break;
        case 7: item.vertices.last().indices[0] = item.oldToNew.size(); break;
        case 8: item.oldToNew.removeLast(); break;
        case 9: item.oldToNew[1] = item.oldToNew[0]; break;
        case 10: item.oldToNew[0] = item.vertices.size() - 1; break;
        case 11: item.oldToNew[0] = -2; break;
        case 12: item.indices.removeLast(); break;
        case 13: item.indices[0] = quint16(item.vertices.size()); break;
        case 14: request.gluePolicy = Builder::GlueEndpointPolicy(99); break;
        case 15: request.glueRetargets.append({0,0,Builder::GlueRetarget::A,0}); break;
        case 16: item.vertices.last().weights = {std::numeric_limits<double>::infinity(), 0}; break;
        case 17: item.vertices = QVector<Builder::VertexSource>(Builder::MaximumVertices + 1); break;
        case 18: item.vertices.last().weights = {1}; break;
        }
        Builder::Result result; result.bytes = "sentinel"; result.state.columns.insert(0x178, "keep");
        Builder::Impact impact; impact.blockingReasons = {"sentinel"}; QString error;
        QVERIFY(!Builder::Analyze(bytes, request, impact, &error)); QCOMPARE(impact.blockingReasons, QStringList{"sentinel"}); QVERIFY(!error.isEmpty());
        QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel")); QCOMPARE(result.state.columns.value(0x178), QByteArray("keep"));
        QCOMPARE(bytes, original); QCOMPARE(source("Mark"), original);
    }

    void requestWeightsAndOutputPoolsHaveBudgets() {
        const auto bytes = source("Mark"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        Builder::Request request; auto edit = identity(table, 0);
        edit.vertices.append({QVector<int>(1000001, 0), QVector<double>(1000001, 1.0 / 1000001)}); request.meshes.append(edit);
        Builder::Result result; result.bytes = "sentinel"; QString error;
        QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel"));
        request.meshes.clear();
        for (int index = 0; index < 2; ++index) {
            auto weighted = identity(table, index);
            weighted.vertices.append({QVector<int>(500001, 0), QVector<double>(500001, 1.0 / 500001)});
            request.meshes.append(std::move(weighted));
        }
        QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel")); QVERIFY(error.contains(QStringLiteral("总数")));
        const auto manyBytes = source("Natori"); auto manyReader = CReader::FromBytes(manyBytes); CSectionTable many(manyReader); QVERIFY(many.IsValid());
        int selected = -1;
        for (int i = 0; i < many.art_meshes.size(); ++i) if (many.art_meshes[i]->keyform_sources_counts >= 16 && many.art_meshes[i]->vertex_counts >= 2) { selected = i; break; }
        QVERIFY(selected >= 0); edit = identity(many, selected);
        while (edit.vertices.size() < Builder::MaximumVertices) edit.vertices.append({{0,1},{.5,.5}});
        request.meshes = {edit}; QVERIFY(!Builder::Build(manyBytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel")); QVERIFY(error.contains(QStringLiteral("关键形位置")));
    }

    void convexWeightsKeepFiniteExtremeConstants_data() {
        QTest::addColumn<float>("value");
        QTest::newRow("positive") << std::numeric_limits<float>::max();
        QTest::newRow("negative") << -std::numeric_limits<float>::max();
    }
    void convexWeightsKeepFiniteExtremeConstants() {
        QFETCH(float, value); auto bytes = source("Mark"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto* mesh = table.art_meshes.first(); quint32 bits; std::memcpy(&bits, &value, 4);
        qToLittleEndian(bits, bytes.data() + table.sectionOffsetTable->uvs->uvs + mesh->uv_sources_begin_indices * 4);
        for (int row = 0; row < mesh->keyform_sources_counts; ++row) {
            const int first = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            qToLittleEndian(bits, bytes.data() + table.sectionOffsetTable->keyform_positions->xys + first * 4);
        }
        auto edit = identity(table, 0); edit.vertices.append({QVector<int>(6,0), QVector<double>(6,1.0/6)});
        Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        const auto* changed = after.art_meshes.first();
        QCOMPARE(after.uvs->uvs[changed->uv_sources_begin_indices + mesh->vertex_counts * 2], value);
        for (int row = 0; row < mesh->keyform_sources_counts; ++row) {
            const int first = after.art_mesh_keyforms[changed->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            QCOMPARE(after.keyform_positions->xys[first + mesh->vertex_counts * 2], value);
        }
    }

    void acceptedWeightToleranceDoesNotChangeRetainedBits() {
        const auto bytes = source("Mark"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        auto edit = identity(table, 0); edit.vertices.append({{0,1},{.25, .75 + Builder::WeightTolerance / 2}});
        Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        const auto* beforeMesh = table.art_meshes.first(); const auto* afterMesh = after.art_meshes.first();
        for (int row = 0; row < beforeMesh->keyform_sources_counts; ++row) {
            const int beforeFirst = table.art_mesh_keyforms[beforeMesh->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            const int afterFirst = after.art_mesh_keyforms[afterMesh->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            QCOMPARE(bytes.mid(table.sectionOffsetTable->keyform_positions->xys + beforeFirst * 4, beforeMesh->vertex_counts * 8),
                result.bytes.mid(after.sectionOffsetTable->keyform_positions->xys + afterFirst * 4, beforeMesh->vertex_counts * 8));
        }
        QCOMPARE(bytes.mid(table.sectionOffsetTable->uvs->uvs + beforeMesh->uv_sources_begin_indices * 4, beforeMesh->vertex_counts * 8),
            result.bytes.mid(after.sectionOffsetTable->uvs->uvs + afterMesh->uv_sources_begin_indices * 4, beforeMesh->vertex_counts * 8));
        request.meshes[0].vertices.last().weights = {.25, .75 + Builder::WeightTolerance * 2}; const auto original = result.bytes;
        QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, original);
    }

    void malformedGlueCannotPartiallyPublishMeshChanges() {
        auto bytes = source("Natori"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid()); QVERIFY(!table.glue.isEmpty());
        const auto* glue = table.glue.first(); auto edit = identity(table, glue->art_mesh_indices_a); edit.vertices.append({{0,1},{.5,.5}});
        Builder::Request request; request.meshes.append(edit); Builder::Result result; result.bytes = "sentinel";
        qToLittleEndian(qint32(glue->glue_info_sources_counts - 1), bytes.data() + table.sectionOffsetTable->glue->glue_info_sources_counts);
        const auto original = bytes; QString error; QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel")); QCOMPARE(bytes, original);
        QVERIFY(error.contains(QStringLiteral("Glue")));
    }

    void sharedGlueSlicesHaveATotalTraversalBudget() {
        const auto original = source("Natori"); auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        int glueIndex = -1, a = -1, b = -1;
        for (int index = 0; index < table.glue.size() && glueIndex < 0; ++index) {
            const auto* glue = table.glue[index]; if (glue->glue_info_sources_counts < 10) continue;
            QSet<int> used;
            for (int row = 0; row < glue->glue_info_sources_counts; row += 2)
                used.insert(table.glue_info[glue->glue_info_sources_begin_indices + row]->position_indices);
            for (int vertex = 0; vertex < table.art_meshes[glue->art_mesh_indices_a]->vertex_counts; ++vertex) if (!used.contains(vertex)) {
                if (a < 0) a = vertex; else { b = vertex; break; }
            }
            if (b >= 0) glueIndex = index; else a = -1;
        }
        QVERIFY(glueIndex >= 0); QVERIFY(a >= 0); QVERIFY(b >= 0);
        const auto* glue = table.glue[glueIndex]; const int rows = 1000000 / glue->glue_info_sources_counts + 1;
        CMocParameterStructureBuilder::ColumnState state;
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            if (!QByteArray(span.name).startsWith("glue.")) continue;
            const auto width = CSectionValidation::StorageWidth(span);
            const auto value = original.mid(span.offset + glueIndex * width, width);
            QByteArray column; column.reserve(rows * width);
            for (int row = 0; row < rows; ++row) {
                if (QByteArray(span.name) == "glue.ids") {
                    QByteArray id = QByteArray("RepeatedGlue") + QByteArray::number(row); id.resize(width, '\0'); column.append(id);
                } else column.append(value);
            }
            state.columns.insert(span.slot, column); state.counts.insert(span.countIndex, rows);
        }
        QByteArray bytes; QString error; QVERIFY2(CMocParameterStructureBuilder::BuildColumnState(original, state, bytes, &error), qPrintable(error));
        auto duplicatedReader = CReader::FromBytes(bytes); CSectionTable duplicated(duplicatedReader); QVERIFY(duplicated.IsValid()); QCOMPARE(duplicated.glue.size(), rows);
        Builder::Request request; auto edit = identity(duplicated, glue->art_mesh_indices_a); edit.vertices.append({{a,b},{.5,.5}}); request.meshes.append(edit);
        Builder::Result result;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); QVERIFY(result.glues.isEmpty());
        request.meshes[0] = identity(duplicated, glue->art_mesh_indices_a);
        std::swap(request.meshes[0].vertices[a], request.meshes[0].vertices[b]); std::swap(request.meshes[0].oldToNew[a], request.meshes[0].oldToNew[b]);
        for (auto& index : request.meshes[0].indices) index = request.meshes[0].oldToNew[index];
        const auto accepted = result.bytes; Builder::Impact impact; impact.blockingReasons = {"sentinel"};
        QVERIFY(!Builder::Analyze(bytes, request, impact, &error)); QCOMPARE(impact.blockingReasons, QStringList{"sentinel"});
        QVERIFY(error.contains(QStringLiteral("Glue 配对检查")));
        QVERIFY(!Builder::Build(bytes, request, result, &error)); QCOMPARE(result.bytes, accepted);
    }

    void glueOnlyRemappingDoesNotDetachAliasedUv() {
        auto bytes = source("Natori"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid()); QVERIFY(!table.glue.isEmpty());
        const auto* glue = table.glue.first(); const int target = glue->art_mesh_indices_a, peer = glue->art_mesh_indices_b;
        const auto* mesh = table.art_meshes[target]; const auto* other = table.art_meshes[peer];
        const int uvBegin = std::min(mesh->uv_sources_begin_indices, other->uv_sources_begin_indices);
        qToLittleEndian(qint32(uvBegin), bytes.data() + table.sectionOffsetTable->art_meshes->uv_sources_begin_indices + target * 4);
        qToLittleEndian(qint32(uvBegin), bytes.data() + table.sectionOffsetTable->art_meshes->uv_sources_begin_indices + peer * 4);
        const int commonCount = std::min(mesh->vertex_counts, other->vertex_counts);
        QVERIFY(commonCount > 1);
        auto edit = identity(table, target);
        const int removed = table.glue_info[glue->glue_info_sources_begin_indices]->position_indices, replacement = removed ? 0 : 1;
        edit.oldToNew[removed] = -1;
        Builder::Request request; request.meshes.append(edit); request.gluePolicy = Builder::GlueEndpointPolicy::Retarget;
        for (int index = 0; index < table.glue.size(); ++index) {
            const auto* object = table.glue[index];
            for (int pair = 0; pair < object->glue_info_sources_counts / 2; ++pair) for (int side = 0; side < 2; ++side)
                if ((side ? object->art_mesh_indices_b : object->art_mesh_indices_a) == target &&
                    table.glue_info[object->glue_info_sources_begin_indices + pair * 2 + side]->position_indices == removed)
                    request.glueRetargets.append({index, pair, side ? Builder::GlueRetarget::B : Builder::GlueRetarget::A, replacement});
        }
        Builder::Impact impact; Builder::Result result; QString error;
        QVERIFY2(Builder::Analyze(bytes, request, impact, &error), qPrintable(error)); QVERIFY(impact.blockingReasons.isEmpty());
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); QVERIFY(!result.glues.isEmpty());
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info))
            if (QByteArray(span.name).startsWith("art_meshes.") || QByteArray(span.name) == "uvs.uvs" || QByteArray(span.name) == "position_indices.indices")
                QVERIFY(!result.state.columns.contains(span.slot));
    }

    void manyDistinctKeyformSlicesKeepPrefixAndOrder() {
        const auto original = source("Mark"); auto reader = CReader::FromBytes(original); CSectionTable table(reader); QVERIFY(table.IsValid());
        int target = -1;
        for (int index = 0; index < table.art_meshes.size(); ++index)
            if (table.art_meshes[index]->vertex_counts >= 2 && (target < 0 || table.art_meshes[index]->vertex_counts < table.art_meshes[target]->vertex_counts)) target = index;
        QVERIFY(target >= 0); const auto* mesh = table.art_meshes[target]; const int count = 4096;
        const int oldRows = table.art_mesh_keyforms.size(), oldPositions = table.keyform_positions->xys.size();
        const int positionBegin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
        CMocParameterStructureBuilder::ColumnState state;
        for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
            const QByteArray name(span.name); auto column = original.mid(span.offset, span.count * span.width);
            if (name.startsWith("art_mesh_keyforms.")) {
                if (!span.offset && span.optional) continue;
                const auto value = column.mid(mesh->keyform_sources_begin_indices * span.width, span.width);
                for (int row = 0; row < count; ++row) {
                    if (name == "art_mesh_keyforms.keyform_position_sources_begin_indices") {
                        char field[4]; qToLittleEndian(qint32(oldPositions + row * mesh->vertex_counts * 2), field); column.append(field, 4);
                    } else column.append(value);
                }
                state.counts.insert(span.countIndex, oldRows + count);
            } else if (name == "keyform_positions.xys") {
                const auto values = column.mid(positionBegin * 4, mesh->vertex_counts * 8);
                for (int row = 0; row < count; ++row) column.append(values);
                state.counts.insert(span.countIndex, oldPositions + count * mesh->vertex_counts * 2);
            } else if (name == "art_meshes.keyform_sources_begin_indices") qToLittleEndian(qint32(oldRows), column.data() + target * 4);
            else if (name == "art_meshes.keyform_sources_counts") qToLittleEndian(qint32(count), column.data() + target * 4);
            else continue;
            state.columns.insert(span.slot, std::move(column));
        }
        QByteArray bytes; QString error; QVERIFY2(CMocParameterStructureBuilder::BuildColumnState(original, state, bytes, &error), qPrintable(error));
        auto expandedReader = CReader::FromBytes(bytes); CSectionTable expanded(expandedReader); QVERIFY(expanded.IsValid());
        Builder::Request request; auto edit = identity(expanded, target); edit.vertices.append({{0,1},{.25,.75}}); request.meshes.append(edit);
        Builder::Result result; QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        QCOMPARE(result.bytes.mid(after.sectionOffsetTable->keyform_positions->xys, expanded.keyform_positions->xys.size() * 4),
                 bytes.mid(expanded.sectionOffsetTable->keyform_positions->xys, expanded.keyform_positions->xys.size() * 4));
        const auto* next = after.art_meshes[target]; QCOMPARE(next->keyform_sources_counts, count);
        for (int row = 0; row < count; ++row) {
            const int beforeFirst = expanded.art_mesh_keyforms[expanded.art_meshes[target]->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            const int afterFirst = after.art_mesh_keyforms[next->keyform_sources_begin_indices + row]->keyform_position_sources_begin_indices;
            QCOMPARE(result.bytes.mid(after.sectionOffsetTable->keyform_positions->xys + afterFirst * 4, mesh->vertex_counts * 8),
                     bytes.mid(expanded.sectionOffsetTable->keyform_positions->xys + beforeFirst * 4, mesh->vertex_counts * 8));
            for (int axis = 0; axis < 2; ++axis) QCOMPARE(after.keyform_positions->xys[afterFirst + mesh->vertex_counts * 2 + axis],
                float(.25 * double(expanded.keyform_positions->xys[beforeFirst + axis]) + .75 * double(expanded.keyform_positions->xys[beforeFirst + 2 + axis])));
        }
    }

    void appendVertexRewritesEveryNormalPositionAndUv() {
        const auto bytes = source("Mark"); auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const auto* mesh = table.art_meshes.first(); auto edit = identity(table, 0); QVERIFY(mesh->vertex_counts >= 2);
        edit.vertices.append({{0, 1}, {.25, .75}}); Builder::Request request; request.meshes.append(edit); Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error)); auto candidateReader = CReader::FromBytes(result.bytes); CSectionTable after(candidateReader); QVERIFY(after.IsValid());
        const auto* actual = after.art_meshes.first(); QCOMPARE(actual->vertex_counts, mesh->vertex_counts + 1); QCOMPARE(actual->keyform_sources_counts, mesh->keyform_sources_counts);
        for (int row = 0; row < mesh->keyform_sources_counts; ++row) {
            const auto* old = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices + row]; const auto* next = after.art_mesh_keyforms[actual->keyform_sources_begin_indices + row];
            QCOMPARE(next->opacities, old->opacities); QCOMPARE(next->draw_orders, old->draw_orders);
            for (int at = 0; at < mesh->vertex_counts * 2; ++at) QCOMPARE(after.keyform_positions->xys[next->keyform_position_sources_begin_indices + at], table.keyform_positions->xys[old->keyform_position_sources_begin_indices + at]);
            for (int axis = 0; axis < 2; ++axis) QCOMPARE(after.keyform_positions->xys[next->keyform_position_sources_begin_indices + mesh->vertex_counts * 2 + axis],
                float(.25 * double(table.keyform_positions->xys[old->keyform_position_sources_begin_indices + axis]) + .75 * double(table.keyform_positions->xys[old->keyform_position_sources_begin_indices + 2 + axis])));
        }
        for (int axis = 0; axis < 2; ++axis) QCOMPARE(after.uvs->uvs[actual->uv_sources_begin_indices + mesh->vertex_counts * 2 + axis],
            float(.25 * double(table.uvs->uvs[mesh->uv_sources_begin_indices + axis]) + .75 * double(table.uvs->uvs[mesh->uv_sources_begin_indices + 2 + axis])));
        QByteArray rebuilt; QVERIFY(CMocParameterStructureBuilder::BuildColumnState(bytes, result.state, rebuilt, &error)); QCOMPARE(rebuilt, result.bytes);
        QCOMPARE(source("Mark"), bytes);
    }
};
QTEST_GUILESS_MAIN(MocMeshStructureTests)
#include "mesh_structure_moc_tests.moc"
