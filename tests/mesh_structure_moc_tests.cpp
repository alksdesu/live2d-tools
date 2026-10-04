#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "live2dstruct/csectionvalidation.h"
#include "wide_mesh_fixture.h"
#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>
#include <QtTest>
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
QMap<QString, QByteArray> livePositions(const QByteArray& bytes, const CSectionTable& table, int omittedMesh)
{
    QMap<QString, QByteArray> result;
    const auto slice = [&](const QString& key, int first, int vertices) {
        result.insert(
            key, bytes.mid(table.sectionOffsetTable->keyform_positions->xys + qint64(first) * 4, qint64(vertices) * 8));
    };
    for (int mesh = 0; mesh < table.art_meshes.size(); ++mesh) {
        if (mesh == omittedMesh)
            continue;
        const auto* owner = table.art_meshes[mesh];
        for (int row = 0; row < owner->keyform_sources_counts; ++row)
            slice(QString("mesh/%1/%2").arg(mesh).arg(row),
                table.art_mesh_keyforms[owner->keyform_sources_begin_indices + row]
                    ->keyform_position_sources_begin_indices,
                owner->vertex_counts);
    }
    for (int warp = 0; warp < table.warp_deformers.size(); ++warp) {
        const auto* owner = table.warp_deformers[warp];
        for (int row = 0; row < owner->keyform_sources_counts; ++row)
            slice(QString("warp/%1/%2").arg(warp).arg(row),
                table.warp_deformer_keyforms[owner->keyform_sources_begin_indices + row]
                    ->keyform_position_sources_begin_indices,
                owner->vertex_counts);
    }
    const auto shapes = [&](const auto& owners, bool warp) {
        for (int index = 0; index < owners.size(); ++index) {
            const auto* shape = owners[index];
            if (!warp && shape->target_indices == omittedMesh)
                continue;
            const int vertices = warp ? table.warp_deformers[shape->target_indices]->vertex_counts
                                      : table.art_meshes[shape->target_indices]->vertex_counts;
            for (int binding = 0; binding < shape->blend_shape_keyform_binding_sources_counts; ++binding) {
                const auto* item
                    = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices
                        + binding];
                for (int row = 0; row < item->keyform_sources_blend_shape_counts; ++row) {
                    const int key = item->keyform_sources_blend_shape_indices + row;
                    slice(QString("blend/%1/%2/%3/%4").arg(warp).arg(index).arg(binding).arg(row),
                        warp ? table.warp_deformer_keyforms[key]->keyform_position_sources_begin_indices
                             : table.art_mesh_keyforms[key]->keyform_position_sources_begin_indices,
                        vertices);
                }
            }
        }
    };
    shapes(table.blend_shapes_art_meshes, false);
    shapes(table.blend_shapes_warp_deformers, true);
    return result;
}
QByteArray versionFive(QByteArray bytes)
{
    const auto get = [&](int offset) { return qFromLittleEndian<qint32>(bytes.constData() + offset); };
    const auto put = [&](int offset, int value) { qToLittleEndian(qint32(value), bytes.data() + offset); };
    if (bytes.size() < 0x840 || bytes[4] != 3)
        return { };
    const int counts = get(0x40), canvas = get(0x44);
    bytes.insert(canvas, QByteArray(64, '\0'));
    for (int slot = 0x40; slot < counts; slot += 4)
        if (get(slot) >= canvas)
            put(slot, get(slot) + 64);
    bytes[4] = 5;
    put(counts + 32 * 4, 0);
    put(counts + 33 * 4, 0);
    return bytes;
}
void integerColumn(DeformerReplayFixture::RawColumns& raw, const QByteArray& name, const QVector<int>& values)
{
    const auto span = raw.spans.value(name);
    QByteArray payload;
    for (int value : values) {
        char encoded[4];
        qToLittleEndian(qint32(value), encoded);
        payload.append(encoded, 4);
    }
    raw.state.columns[span.slot] = payload;
    raw.state.counts[span.countIndex] = values.size();
}
QVector<int> poolCounts(const CSectionTable& table)
{
    return { int(table.uvs->uvs.size()), int(table.position_indices->indices.size()),
        int(table.keyform_positions->xys.size()), int(table.art_mesh_keyforms.size()),
        int(table.blend_shape_keyform_bindings.size()), int(table.glue_info.size()) };
}
}
class MocMeshStructureTests : public QObject
{
    Q_OBJECT
private slots:
    void repeatedWideEditsReuseEveryPoolWithoutChangingOtherOwners()
    {
        WideMeshData::Fixture fixture;
        QString error;
        QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Grid, fixture, &error, 1, 2), qPrintable(error));
        QByteArray current = fixture.bytes;
        QVector<int> stableCounts;
        for (int round = 0; round < 12; ++round) {
            for (bool insert : { false, true }) {
                const auto original = current;
                auto reader = CReader::FromBytes(current);
                CSectionTable table(reader);
                QVERIFY(table.IsValid());
                int target = -1;
                for (int i = 0; i < table.art_meshes.size(); ++i)
                    if (table.art_meshes[i]->ids == fixture.ids.first())
                        target = i;
                QVERIFY(target >= 0);
                auto edit = identity(table, target);
                const auto siblings = livePositions(current, table, target);
                if (insert) {
                    edit.vertices.append({ { 65278, 65279, 65534 }, { .25, .25, .5 } });
                    edit.indices.append({ 65533, 65534, 65535 });
                } else {
                    edit.vertices.removeLast();
                    edit.oldToNew.last() = -1;
                    QVector<quint16> kept;
                    for (int i = 0; i < edit.indices.size(); i += 3)
                        if (edit.indices[i] != 65535 && edit.indices[i + 1] != 65535 && edit.indices[i + 2] != 65535)
                            kept.append({ edit.indices[i], edit.indices[i + 1], edit.indices[i + 2] });
                    edit.indices = kept;
                }
                Builder::Request request;
                request.meshes = { edit };
                Builder::Impact impact;
                QVERIFY2(Builder::Analyze(current, request, impact, &error), qPrintable(error));
                QVERIFY(impact.blockingReasons.isEmpty());
                Builder::Result result;
                QVERIFY2(Builder::Build(current, request, result, &error), qPrintable(error));
                QCOMPARE(current, original);
                auto afterReader = CReader::FromBytes(result.bytes);
                CSectionTable after(afterReader);
                QVERIFY(after.IsValid());
                QCOMPARE(livePositions(result.bytes, after, target), siblings);
                QCOMPARE(after.art_meshes[target]->vertex_counts, insert ? 65536 : 65535);
                const auto counts = poolCounts(after);
                if (round)
                    QCOMPARE(counts, stableCounts);
                if (insert && !round)
                    stableCounts = counts;
                current = result.bytes;
            }
        }
    }

    void fragmentedFreeSpaceRejectsAtomically()
    {
        const auto original = source("Mark");
        auto reader = CReader::FromBytes(original);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        DeformerReplayFixture::RawColumns raw(original);
        const auto span = raw.spans.value("position_indices.indices");
        raw.state.columns[span.slot] = QByteArray(2000000, '\0');
        raw.state.counts[span.countIndex] = 1000000;
        for (int i = 0; i < table.art_meshes.size(); ++i) {
            raw.set("art_meshes.position_index_sources_begin_indices", i,
                i == 0 ? 0 : int(qint64(i) * 1000000 / table.art_meshes.size()));
            raw.set("art_meshes.position_index_sources_counts", i, 3);
        }
        QString error;
        const auto bytes = raw.encode(&error);
        QVERIFY2(!bytes.isEmpty(), qPrintable(error));
        auto loadedReader = CReader::FromBytes(bytes);
        CSectionTable loaded(loadedReader);
        auto edit = identity(loaded, 0);
        edit.indices.clear();
        for (int i = 0; i < 130050; ++i)
            edit.indices.append({ 0, 1, 2 });
        Builder::Request request;
        request.meshes = { edit };
        Builder::Result result;
        result.bytes = "sentinel";
        QVERIFY(!Builder::Build(bytes, request, result, &error));
        QVERIFY(error.contains(QStringLiteral("连续空间")));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        Builder::Impact impact;
        impact.blockingReasons = { "sentinel" };
        QVERIFY(!Builder::Analyze(bytes, request, impact, &error));
        QCOMPARE(impact.blockingReasons, QStringList { "sentinel" });
    }

    void sharedBlendBindingsStayProtectedForEveryOwnerType_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 4; ++kind)
            QTest::newRow(qPrintable(QString::number(kind))) << kind;
    }
    void sharedBlendBindingsStayProtectedForEveryOwnerType()
    {
        QFETCH(int, kind);
        const auto base = versionFive(source("Mark"));
        auto baseReader = CReader::FromBytes(base);
        CSectionTable original(baseReader);
        QVERIFY2(original.IsValid(), qPrintable(baseReader.Error()));
        int target = 0;
        for (int i = 1; i < original.art_meshes.size(); ++i)
            if (original.art_meshes[i]->vertex_counts < original.art_meshes[target]->vertex_counts)
                target = i;
        const int peer = target == 0 ? 1 : 0;
        DeformerReplayFixture::RawColumns raw(base);
        integerColumn(raw, "blend_shape_keyform_bindings.parameter_binding_sources_indices", { 0 });
        integerColumn(raw, "blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", { 0 });
        integerColumn(raw, "blend_shape_keyform_bindings.keyform_sources_blend_shape_counts", { 1 });
        integerColumn(raw, "blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices", { 0 });
        integerColumn(raw, "blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts", { 0 });
        const QVector<QByteArray> groups { "blend_shapes_art_meshes", "blend_shapes_warp_deformers",
            "blend_shapes_parts", "blend_shapes_rotation_deformers" };
        for (int i = 0; i < groups.size(); ++i) {
            QVector<int> targets;
            if (i == 0)
                targets.append(target);
            if (i == kind)
                targets.append(i == 0 ? peer : 0);
            integerColumn(raw, groups[i] + ".target_indices", targets);
            integerColumn(
                raw, groups[i] + ".blend_shape_keyform_binding_sources_begin_indices", QVector<int>(targets.size(), 0));
            integerColumn(
                raw, groups[i] + ".blend_shape_keyform_binding_sources_counts", QVector<int>(targets.size(), 1));
        }
        QString error;
        QByteArray current = raw.encode(&error);
        QVERIFY2(!current.isEmpty(), qPrintable(error));
        int stableBindings = -1, stableRows = -1;
        for (int round = 0; round < 12; ++round) {
            auto reader = CReader::FromBytes(current);
            CSectionTable table(reader);
            QVERIFY(table.IsValid());
            const auto siblings = livePositions(current, table, target);
            auto edit = identity(table, target);
            edit.vertices.append({ { 0, 1 }, { .25, .75 } });
            Builder::Request request;
            request.meshes = { edit };
            Builder::Result result;
            QVERIFY2(Builder::Build(current, request, result, &error), qPrintable(error));
            auto afterReader = CReader::FromBytes(result.bytes);
            CSectionTable after(afterReader);
            QVERIFY(after.IsValid());
            QCOMPARE(livePositions(result.bytes, after, target), siblings);
            const int rewritten
                = after.blend_shapes_art_meshes.first()->blend_shape_keyform_binding_sources_begin_indices;
            QVERIFY(rewritten != 0);
            const DeformerReplayFixture::RawColumns afterColumns(result.bytes);
            for (const auto& span : CSectionValidation::ArraySpans(*table.sectionOffsetTable, *table.count_info)) {
                if (!QByteArray(span.name).startsWith("blend_shape_keyform_bindings."))
                    continue;
                const auto newOffset = afterColumns.spans.value(span.name).offset;
                QCOMPARE(result.bytes.mid(newOffset, span.width), current.mid(span.offset, span.width));
            }
            if (round) {
                QCOMPARE(after.blend_shape_keyform_bindings.size(), stableBindings);
                QCOMPARE(after.art_mesh_keyforms.size(), stableRows);
            } else {
                stableBindings = after.blend_shape_keyform_bindings.size();
                stableRows = after.art_mesh_keyforms.size();
            }
            current = result.bytes;
        }
    }

    void repeatedGlueRemapsReuseTheCoupledPool()
    {
        QByteArray current = source("Natori");
        QVector<int> stableCounts;
        for (int round = 0; round < 12; ++round) {
            auto reader = CReader::FromBytes(current);
            CSectionTable table(reader);
            QVERIFY(table.IsValid());
            QVERIFY(!table.glue.isEmpty());
            const int target = table.glue.first()->art_mesh_indices_a;
            auto edit = identity(table, target);
            QVERIFY(edit.vertices.size() >= 2);
            std::swap(edit.vertices[0], edit.vertices[1]);
            std::swap(edit.oldToNew[0], edit.oldToNew[1]);
            for (auto& index : edit.indices)
                index = edit.oldToNew[index];
            Builder::Request request;
            request.meshes = { edit };
            Builder::Result result;
            QString error;
            QVERIFY2(Builder::Build(current, request, result, &error), qPrintable(error));
            auto afterReader = CReader::FromBytes(result.bytes);
            CSectionTable after(afterReader);
            QVERIFY(after.IsValid());
            if (round)
                QCOMPARE(poolCounts(after), stableCounts);
            else
                stableCounts = poolCounts(after);
            for (int glue = 0; glue < table.glue.size(); ++glue) {
                const auto* before = table.glue[glue];
                const auto* next = after.glue[glue];
                QCOMPARE(next->glue_info_sources_counts, before->glue_info_sources_counts);
                for (int i = 0; i < before->glue_info_sources_counts; ++i) {
                    const auto* a = table.glue_info[before->glue_info_sources_begin_indices + i];
                    const auto* b = after.glue_info[next->glue_info_sources_begin_indices + i];
                    const bool affected = (i % 2 ? before->art_mesh_indices_b : before->art_mesh_indices_a) == target;
                    QCOMPARE(b->position_indices, affected ? edit.oldToNew[a->position_indices] : a->position_indices);
                    QCOMPARE(b->weights, a->weights);
                }
            }
            current = result.bytes;
        }
    }

    void aLongerWarpSliceProtectsTheFullSharedPositionRange()
    {
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes);
        CSectionTable original(reader);
        int target = -1, warpIndex = -1, shared = -1;
        for (int mesh = 0; mesh < original.art_meshes.size() && target < 0; ++mesh) {
            const auto* art = original.art_meshes[mesh];
            if (art->vertex_counts < 2 || art->keyform_sources_counts <= 0)
                continue;
            const int begin = original.art_mesh_keyforms[art->keyform_sources_begin_indices]
                                  ->keyform_position_sources_begin_indices;
            for (int warp = 0; warp < original.warp_deformers.size(); ++warp) {
                const auto* owner = original.warp_deformers[warp];
                if (owner->vertex_counts > art->vertex_counts && owner->keyform_sources_counts > 0
                    && begin + owner->vertex_counts * 2 <= original.keyform_positions->xys.size()) {
                    target = mesh;
                    warpIndex = warp;
                    shared = begin;
                    break;
                }
            }
        }
        QVERIFY(target >= 0);
        const int row = original.warp_deformers[warpIndex]->keyform_sources_begin_indices;
        qToLittleEndian(qint32(shared),
            bytes.data() + original.sectionOffsetTable->warp_deformer_keyforms->keyform_position_sources_begin_indices
                + row * 4);
        auto changedReader = CReader::FromBytes(bytes);
        CSectionTable changed(changedReader);
        QVERIFY(changed.IsValid());
        const auto others = livePositions(bytes, changed, target);
        auto edit = identity(changed, target);
        edit.vertices.append({ { 0, 1 }, { .25, .75 } });
        Builder::Request request;
        request.meshes = { edit };
        Builder::Result result;
        QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        auto afterReader = CReader::FromBytes(result.bytes);
        CSectionTable after(afterReader);
        QVERIFY(after.IsValid());
        QCOMPARE(livePositions(result.bytes, after, target), others);
        const int preserved = after.warp_deformer_keyforms[row]->keyform_position_sources_begin_indices;
        QCOMPARE(preserved, shared);
        QCOMPARE(result.bytes.mid(after.sectionOffsetTable->keyform_positions->xys + shared * 4,
                     after.warp_deformers[warpIndex]->vertex_counts * 8),
            bytes.mid(changed.sectionOffsetTable->keyform_positions->xys + shared * 4,
                changed.warp_deformers[warpIndex]->vertex_counts * 8));
    }

    void malformedWarpReadDomainCannotBeReclaimed()
    {
        auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes);
        CSectionTable table(reader);
        QVERIFY(!table.warp_deformers.isEmpty());
        qToLittleEndian(qint32(1), bytes.data() + table.sectionOffsetTable->warp_deformers->vertex_counts);
        auto edit = identity(table, 0);
        edit.vertices.append({ { 0, 1 }, { .5, .5 } });
        Builder::Request request;
        request.meshes = { edit };
        Builder::Result result;
        result.bytes = "sentinel";
        QString error;
        QVERIFY(!Builder::Build(bytes, request, result, &error));
        QVERIFY(error.contains(QStringLiteral("Warp 顶点数")));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
    }

    void unsignedCapacityPreservesIdentityAndRejectsOverflow()
    {
        WideMeshData::Fixture fixture;
        QString error;
        QVERIFY2(WideMeshData::make(65536, WideMeshData::Layout::Grid, fixture, &error), qPrintable(error));
        auto reader = CReader::FromBytes(fixture.bytes);
        CSectionTable table(reader);
        QVERIFY(table.IsValid());
        int target = -1;
        for (int i = 0; i < table.art_meshes.size(); ++i)
            if (table.art_meshes[i]->ids == fixture.ids.first())
                target = i;
        QVERIFY(target >= 0);
        const auto edit = identity(table, target);
        QCOMPARE(edit.vertices.size(), 65536);
        QCOMPARE(edit.indices.size(), 390150);
        QVERIFY(edit.indices.contains(65535));
        Builder::Request request;
        request.meshes = { edit };
        Builder::Result result;
        QVERIFY2(Builder::Build(fixture.bytes, request, result, &error), qPrintable(error));
        QCOMPARE(result.bytes, fixture.bytes);
        request.meshes.first().vertices.append({ { 65535 }, { 1 } });
        result.bytes = "sentinel";
        QVERIFY(!Builder::Build(fixture.bytes, request, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
        QVERIFY2(WideMeshData::make(65537, WideMeshData::Layout::Sparse, fixture, &error), qPrintable(error));
        reader = CReader::FromBytes(fixture.bytes);
        CSectionTable larger(reader);
        QVERIFY(larger.IsValid());
        for (int i = 0; i < larger.art_meshes.size(); ++i)
            if (larger.art_meshes[i]->ids == fixture.ids.first())
                target = i;
        request.meshes = { identity(larger, target) };
        QVERIFY(!Builder::Build(fixture.bytes, request, result, &error));
        QCOMPARE(result.bytes, QByteArray("sentinel"));
    }

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

    void manyDistinctKeyformSlicesKeepLiveValuesAndOrder()
    {
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
        QCOMPARE(livePositions(result.bytes, after, target), livePositions(bytes, expanded, target));
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

    void cloneArtMeshCopiesPoolsAndLeavesSourceUntouched() {
        const auto bytes = source("Mark");
        auto reader = CReader::FromBytes(bytes); CSectionTable before(reader); QVERIFY(before.IsValid());
        QVERIFY(!before.art_meshes.isEmpty());
        const auto* sourceMesh = before.art_meshes.first();
        const QByteArray sourceDigest = QCryptographicHash::hash(bytes, QCryptographicHash::Sha256);
        Builder::Result result; QString error;
        QVERIFY2(Builder::Clone(bytes, {sourceMesh->ids, "CloneMesh", -2, -2, -2}, result, &error), qPrintable(error));
        QCOMPARE(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256), sourceDigest);
        auto nextReader = CReader::FromBytes(result.bytes); CSectionTable after(nextReader); QVERIFY(after.IsValid());
        QCOMPARE(after.art_meshes.size(), before.art_meshes.size() + 1);
        const auto* clone = after.art_meshes.last();
        QCOMPARE(clone->ids, QStringLiteral("CloneMesh"));
        QCOMPARE(clone->vertex_counts, sourceMesh->vertex_counts);
        QCOMPARE(clone->position_index_sources_counts, sourceMesh->position_index_sources_counts);
        QCOMPARE(clone->keyform_sources_counts, sourceMesh->keyform_sources_counts);
        QVector<float> actualUvs, sourceUvs;
        for (int index = 0; index < clone->vertex_counts * 2; ++index) {
            actualUvs.append(after.uvs->uvs[clone->uv_sources_begin_indices + index]);
            sourceUvs.append(before.uvs->uvs[sourceMesh->uv_sources_begin_indices + index]);
        }
        QCOMPARE(actualUvs, sourceUvs);
        QByteArray rebuilt;
        QVERIFY2(CMocParameterStructureBuilder::BuildColumnState(bytes, result.state, rebuilt, &error), qPrintable(error));
        QCOMPARE(rebuilt, result.bytes);
    }

    void glueWeightEditsAreAtomicAndPreserveTopology() {
        const auto bytes = source("Rice");
        auto reader = CReader::FromBytes(bytes); CSectionTable before(reader); QVERIFY(before.IsValid());
        QVERIFY(!before.glue.isEmpty());
        const auto* glue = before.glue.first();
        QVERIFY(glue->glue_info_sources_counts >= 2);
        const int row = glue->glue_info_sources_begin_indices;
        const float original = before.glue_info[row]->weights;
        const float replacement = original == .25f ? .75f : .25f;
        const float replacementB = before.glue_info[row + 1]->weights == .25f ? .5f : .25f;
        Builder::Request request;
        request.glueWeights.append({0, 0, Builder::GlueRetarget::A, replacement});
        request.glueWeights.append({0, 0, Builder::GlueRetarget::B, replacementB});
        Builder::Result result; QString error;
        QVERIFY2(Builder::Build(bytes, request, result, &error), qPrintable(error));
        QCOMPARE(bytes, source("Rice"));
        auto afterReader = CReader::FromBytes(result.bytes); CSectionTable after(afterReader); QVERIFY(after.IsValid());
        QCOMPARE(after.art_meshes.size(), before.art_meshes.size());
        QCOMPARE(after.art_meshes.first()->vertex_counts, before.art_meshes.first()->vertex_counts);
        QCOMPARE(after.glue.first()->glue_info_sources_counts, glue->glue_info_sources_counts);
        QCOMPARE(after.glue_info[after.glue.first()->glue_info_sources_begin_indices]->position_indices,
                 before.glue_info[row]->position_indices);
        QCOMPARE(after.glue_info[after.glue.first()->glue_info_sources_begin_indices]->weights, replacement);
        QCOMPARE(after.glue_info[after.glue.first()->glue_info_sources_begin_indices + 1]->weights, replacementB);
        QByteArray rebuilt;
        QVERIFY2(CMocParameterStructureBuilder::BuildColumnState(bytes, result.state, rebuilt, &error), qPrintable(error));
        QCOMPARE(rebuilt, result.bytes);
        Builder::Request duplicate = request;
        duplicate.glueWeights.append({0, 0, Builder::GlueRetarget::A, .5f});
        Builder::Result rejected; rejected.bytes = "sentinel";
        QVERIFY(!Builder::Build(bytes, duplicate, rejected, &error));
        QCOMPARE(rejected.bytes, QByteArray("sentinel"));
        for (float weight : {-0.1f, 1.1f, std::numeric_limits<float>::quiet_NaN()}) {
            Builder::Request invalid;
            invalid.glueWeights.append({0, 0, Builder::GlueRetarget::A, weight});
            rejected.bytes = "sentinel";
            QVERIFY(!Builder::Build(bytes, invalid, rejected, &error));
            QCOMPARE(rejected.bytes, QByteArray("sentinel"));
        }
    }
};
QTEST_GUILESS_MAIN(MocMeshStructureTests)
#include "mesh_structure_moc_tests.moc"
