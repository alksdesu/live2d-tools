#include "deformer_replay_fixture.h"
#include <QtTest>
#include <limits>
#include <QElapsedTimer>
using namespace live2dstruct;
using namespace DeformerReplayFixture;
class DeformerStructureReplayTests : public QObject
{
    Q_OBJECT
private slots:
    void rootInsertionAndRemoval_data()
    {
        QTest::addColumn<bool>("warp"); QTest::newRow("warp") << true; QTest::newRow("rotation") << false;
    }
    void rootInsertionAndRemoval()
    {
        QFETCH(bool, warp); const auto base = read("Mark"); QVERIFY(!base.isEmpty());
        const auto insertion = rootMesh(base, warp); QVERIFY(!insertion.children.isEmpty());
        CDeformerStructureReplay::Impact impact; QString error;
        QVERIFY2(CDeformerStructureReplay::Analyze(base, insertRequest(base, insertion), impact, &error), qPrintable(error));
        QVERIFY2(impact.blockingReasons.isEmpty(), qPrintable(impact.blockingReasons.join('\n')));
        CMocDeformerStructureBuilder::Result added, removed;
        QVERIFY2(CDeformerStructureReplay::Build(base, insertRequest(base, insertion), added, &error), qPrintable(error));
        QVERIFY2(CDeformerStructureReplay::Build(added.bytes, removeRequest(added.bytes, insertion.id), removed, &error), qPrintable(error));
        auto reader = CReader::FromBytes(removed.bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        for (const auto* node : table.deformers) QVERIFY(node->ids != insertion.id);
    }
    void sharedSpecificIsDetachedWithoutChangingOldRows()
    {
        const auto original = read("Mark"); auto r = CReader::FromBytes(original); CSectionTable source(r); RawColumns raw(original);
        int first = -1, second = -1;
        for (int i = 0; i < source.deformers.size(); ++i) if (source.deformers[i]->types == 1) { if (first < 0) first = i; else { second = i; break; } }
        QVERIFY(first >= 0 && second >= 0); const int specific = source.deformers[first]->specific_sources_indices;
        raw.set("deformers.specific_sources_indices", second, specific);
        raw.set("deformers.keyform_binding_sources_indices", second, source.deformers[first]->keyform_binding_sources_indices);
        raw.set("deformers.parent_deformer_indices", first, -1); raw.set("deformers.parent_deformer_indices", second, -1);
        const auto base = raw.encode(); QVERIFY(!base.isEmpty());
        CMocDeformerStructureBuilder::InsertRequest insertion; insertion.id = "SharedSourceCage";
        insertion.partIndex = source.deformers[first]->parent_part_indices;
        insertion.children = {{CMocDeformerStructureBuilder::Owner::Rotation, source.deformers[first]->ids}};
        insertion.shape = CMocDeformerStructureBuilder::Warp{1, 1, false, {-1,-1,1,-1,-1,1,1,1}};
        CMocDeformerStructureBuilder::Result result; QString error;
        QVERIFY2(CDeformerStructureReplay::Build(base, insertRequest(base, insertion), result, &error), qPrintable(error));
        RawColumns before(base), after(result.bytes);
        QCOMPARE(after.integer("deformers.specific_sources_indices", second), specific);
        const int nextSpecific = after.integer("deformers.specific_sources_indices", first); QVERIFY(nextSpecific != specific);
        const int oldBegin = before.integer("rotation_deformers.keyform_sources_begin_indices", specific);
        const int nextBegin = after.integer("rotation_deformers.keyform_sources_begin_indices", nextSpecific);
        const int count = before.integer("rotation_deformers.keyform_sources_counts", specific);
        for (auto it = before.spans.cbegin(); it != before.spans.cend(); ++it) if (it.key().startsWith("rotation_deformer_keyforms.") || it.key().startsWith("rotation_deformers."))
            QCOMPARE(after.column(it.key()).left(before.column(it.key()).size()), before.column(it.key()));
        for (const auto* field : {"angles", "scales", "opacities", "is_reflect_x", "is_reflect_y"}) {
            const QByteArray name = QByteArray("rotation_deformer_keyforms.") + field;
            QCOMPARE(after.column(name).mid(nextBegin * 4, count * 4), before.column(name).mid(oldBegin * 4, count * 4));
        }
        for (int k = 0; k < count; ++k) {
            QCOMPARE(after.number("rotation_deformer_keyforms.origin_x", nextBegin + k), float((double(before.number("rotation_deformer_keyforms.origin_x", oldBegin + k)) + 1) / 2));
            QCOMPARE(after.number("rotation_deformer_keyforms.origin_y", nextBegin + k), float((double(before.number("rotation_deformer_keyforms.origin_y", oldBegin + k)) + 1) / 2));
        }
    }
    void ordinaryAndBlendSlicesUseSeparateCoordinates()
    {
        const auto mao = read("Mao"); if (mao.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to official Mao.moc3");
        auto reader = CReader::FromBytes(mao); CSectionTable source(reader); QVERIFY(!source.blend_shapes_art_meshes.empty());
        const auto* shape = source.blend_shapes_art_meshes.front(); const int mesh = shape->target_indices;
        const auto* binding = source.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices];
        const int ordinary = source.art_meshes[mesh]->keyform_sources_begin_indices, blend = binding->keyform_sources_blend_shape_indices;
        RawColumns raw(mao); const int shared = raw.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", blend);
        raw.set("art_meshes.parent_deformer_indices", mesh, -1); raw.set("art_mesh_keyforms.keyform_position_sources_begin_indices", ordinary, shared);
        const auto base = raw.encode(); QVERIFY(!base.isEmpty());
        CMocDeformerStructureBuilder::InsertRequest insertion; insertion.id = "DeltaCage"; insertion.partIndex = source.art_meshes[mesh]->parent_part_indices;
        insertion.children = {{CMocDeformerStructureBuilder::Owner::ArtMesh, source.art_meshes[mesh]->ids}};
        insertion.shape = CMocDeformerStructureBuilder::Warp{1, 1, false, {-1,-1,1,-1,-1,1,1,1}};
        CMocDeformerStructureBuilder::Result result; QString error;
        QVERIFY2(CDeformerStructureReplay::Build(base, insertRequest(base, insertion), result, &error), qPrintable(error));
        RawColumns before(base), after(result.bytes);
        const int next = after.integer("art_meshes.keyform_sources_begin_indices", mesh);
        const int nextShapeBinding = after.integer("blend_shapes_art_meshes.blend_shape_keyform_binding_sources_begin_indices", 0);
        const int nextBlend = after.integer("blend_shape_keyform_bindings.keyform_sources_blend_shape_indices", nextShapeBinding);
        const int normalPosition = after.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", next);
        const int deltaPosition = after.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", nextBlend);
        QVERIFY(normalPosition != deltaPosition);
        const int length = source.art_meshes[mesh]->vertex_counts * 2;
        for (int k = 0; k < length; ++k) {
            const float value = before.number("keyform_positions.xys", shared + k);
            QCOMPARE(after.number("keyform_positions.xys", normalPosition + k), float((double(value) + 1) / 2));
            QCOMPARE(after.number("keyform_positions.xys", deltaPosition + k), float(double(value) / 2));
        }
        for (auto it = before.spans.cbegin(); it != before.spans.cend(); ++it) if (it.key().startsWith("art_mesh_keyforms.") || it.key() == "keyform_positions.xys")
            QCOMPARE(after.column(it.key()).left(before.column(it.key()).size()), before.column(it.key()));
        for (auto it = before.spans.cbegin(); it != before.spans.cend(); ++it) if (it.key().startsWith("keyform_multiply_colors.") || it.key().startsWith("keyform_screen_colors."))
            QCOMPARE(after.column(it.key()).left(before.column(it.key()).size()), before.column(it.key()));
    }
    void unsupportedParentsAndRemovalAreSpecific()
    {
        const auto original = read("Mark"); QString error;
        for (float scale : {0.f, std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()}) {
            const auto base = fixedRotation(original, "Rotation7", scale, 0); QVERIFY(!base.isEmpty());
            auto insertion = underParent(base, "Rotation7", false); QVERIFY(!insertion.children.isEmpty());
            CDeformerStructureReplay::Impact impact;
            QVERIFY(CDeformerStructureReplay::Analyze(base, insertRequest(base, insertion), impact, &error));
            QVERIFY2(!impact.blockingReasons.isEmpty(), qPrintable(error));
            CMocDeformerStructureBuilder::Result result; result.bytes = "sentinel";
            QVERIFY(!CDeformerStructureReplay::Build(base, insertRequest(base, insertion), result, &error)); QCOMPARE(result.bytes, QByteArray("sentinel"));
        }
        CDeformerStructureReplay::Impact impact;
        QVERIFY(CDeformerStructureReplay::Analyze(original, removeRequest(original, "Warp18"), impact, &error));
        QVERIFY(!impact.blockingReasons.isEmpty());
        const auto insertion = underParent(original, "Warp18", false);
        QVERIFY(CDeformerStructureReplay::Analyze(original, insertRequest(original, insertion), impact, &error));
        QVERIFY(!impact.blockingReasons.isEmpty());
    }
    void sharedSpecificDeepChainReusesVerifiedGeometry()
    {
        using Builder = CMocParameterStructureBuilder;
        Builder::CreateRequest create; create.parameter.id = "ReplayStressAxis";
        Builder::BindingRequest axis{{Builder::Owner::Warp, "Warp18"}, {}};
        for (int i = 0; i < 128; ++i) axis.keys.append(float(-1 + 2.0 * i / 127));
        create.bindings.append(axis); Builder::Result expanded; QString error;
        QVERIFY2(Builder::Create(read("Mark"), create, expanded, &error), qPrintable(error));
        auto reader = CReader::FromBytes(expanded.bytes); CSectionTable table(reader); RawColumns raw(expanded.bytes);
        int target = -1; for (int i = 0; i < table.deformers.size(); ++i) if (table.deformers[i]->ids == "Warp18") target = i;
        QVERIFY(target >= 0); const auto* node = table.deformers[target]; const auto* warp = table.warp_deformers[node->specific_sources_indices];
        QSet<int> positions;
        for (int k = 0; k < warp->keyform_sources_counts; ++k) positions.insert(table.warp_deformer_keyforms[warp->keyform_sources_begin_indices + k]->keyform_position_sources_begin_indices);
        auto pool = raw.column("keyform_positions.xys");
        for (int begin : positions) for (int y = 0; y <= warp->rows; ++y) for (int x = 0; x <= warp->columns; ++x) {
            const int index = begin + 2 * (y * (warp->columns + 1) + x);
            for (int component = 0; component < 2; ++component) {
                const float value = component ? float(double(y) / warp->rows) : float(double(x) / warp->columns);
                quint32 bits; std::memcpy(&bits, &value, 4); qToLittleEndian(bits, pool.data() + (index + component) * 4);
            }
        }
        raw.state.columns[raw.spans.value("keyform_positions.xys").slot] = pool;
        constexpr int depth = 3000; const int oldCount = table.deformers.size();
        for (auto it = raw.spans.cbegin(); it != raw.spans.cend(); ++it) if (it.key().startsWith("deformers.")) {
            auto bytes = raw.column(it.key()); const auto cell = bytes.mid(target * it->width, it->width);
            bytes.reserve(bytes.size() + depth * it->width);
            for (int n = 0; n < depth; ++n) {
                if (it.key() == "deformers.ids") bytes.append(("ProofChain" + QByteArray::number(n)).leftJustified(64, '\0'));
                else if (it.key() == "deformers.parent_deformer_indices" || it.key() == "deformers.parent_part_indices") {
                    QByteArray value(4, '\0'); qToLittleEndian(qint32(it.key().endsWith("parent_part_indices") || n == 0 ? -1 : oldCount + n - 1), value.data()); bytes.append(value);
                } else bytes.append(cell);
            }
            raw.state.columns[it->slot] = bytes;
        }
        raw.state.counts[raw.spans.value("deformers.ids").countIndex] = oldCount + depth;
        raw.set("art_meshes.parent_deformer_indices", 0, oldCount + depth - 1); raw.set("art_meshes.parent_part_indices", 0, -1);
        const auto base = raw.encode(&error); QVERIFY2(!base.isEmpty(), qPrintable(error));
        CMocDeformerStructureBuilder::InsertRequest insertion; insertion.id = "DeepProofCage"; insertion.parentId = "ProofChain2999";
        insertion.children = {{Builder::Owner::ArtMesh, table.art_meshes[0]->ids}};
        insertion.shape = CMocDeformerStructureBuilder::Warp{1, 1, false, {-1,-1,1,-1,-1,1,1,1}};
        QElapsedTimer elapsed; elapsed.start(); CMocDeformerStructureBuilder::Result result;
        QVERIFY2(CDeformerStructureReplay::Build(base, insertRequest(base, insertion), result, &error), qPrintable(error));
        qInfo() << "Shared-specific proof" << depth << "ancestors," << warp->keyform_sources_counts << "forms," << warp->vertex_counts << "vertices; Build ms:" << elapsed.elapsed()
                << "source SHA:" << QCryptographicHash::hash(base, QCryptographicHash::Sha256).toHex()
                << "result SHA:" << QCryptographicHash::hash(result.bytes, QCryptographicHash::Sha256).toHex();
        RawColumns before(base), after(result.bytes);
        const int oldBegin = before.integer("art_meshes.keyform_sources_begin_indices", 0), nextBegin = after.integer("art_meshes.keyform_sources_begin_indices", 0);
        for (int k = 0; k < table.art_meshes[0]->keyform_sources_counts; ++k) {
            const int oldPosition = before.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", oldBegin + k);
            const int nextPosition = after.integer("art_mesh_keyforms.keyform_position_sources_begin_indices", nextBegin + k);
            for (int n = 0; n < table.art_meshes[0]->vertex_counts * 2; ++n)
                QCOMPARE(after.number("keyform_positions.xys", nextPosition + n), float((double(before.number("keyform_positions.xys", oldPosition + n)) + 1) / 2));
        }
        CMocDeformerStructureBuilder::Result repeated;
        QVERIFY(CDeformerStructureReplay::Build(base, insertRequest(base, insertion), repeated, &error)); QCOMPARE(repeated.bytes, result.bytes);
    }
    void staleAndUnsupportedRequestsAreAtomic()
    {
        const auto base = read("Mark"); auto insertion = rootMesh(base, false); auto request = insertRequest(base, insertion);
        CDeformerStructureReplay::Impact impact; impact.blockingReasons = {"sentinel"}; QString error;
        request.expectedAuthoringDigest.fill('x');
        QVERIFY(!CDeformerStructureReplay::Analyze(base, request, impact, &error)); QCOMPARE(impact.blockingReasons, QStringList{"sentinel"});
        request = insertRequest(base, insertion); std::get<CMocDeformerStructureBuilder::Rotation>(std::get<CMocDeformerStructureBuilder::InsertRequest>(request.operation).shape).scale = 0;
        QVERIFY(CDeformerStructureReplay::Analyze(base, request, impact, &error)); QVERIFY(!impact.blockingReasons.isEmpty());
        CMocDeformerStructureBuilder::Result output; output.bytes = "keep";
        QVERIFY(!CDeformerStructureReplay::Build(base, request, output, &error)); QCOMPARE(output.bytes, QByteArray("keep"));
    }
};
QTEST_GUILESS_MAIN(DeformerStructureReplayTests)
#include "deformer_structure_replay_tests.moc"
