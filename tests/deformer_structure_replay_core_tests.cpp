#include "deformer_replay_fixture.h"
#include "real_core_model_fixture.h"
using namespace live2dstruct;
namespace Fixture = DeformerReplayFixture;
using RawBuilder = CMocDeformerStructureBuilder;
namespace {
QString compare(const QByteArray& before, const QByteArray& after)
{
    QByteArray normalized; QString error;
    if (!CMocDeformerOrder::Normalize(after, normalized, &error)) return error;
    CoreModel original, candidate;
    if (!original.load(before) || !candidate.load(normalized)) return "Core rejected source/candidate";
    auto sequence = poses(original.value);
    for (int i = 0; i < sequence[0].size(); ++i) {
        auto pose = sequence[0]; pose[i] = (csmGetParameterMinimumValues(original.value)[i] + pose[i]) * .5f; sequence.append(pose);
        pose = sequence[0]; pose[i] = (csmGetParameterMaximumValues(original.value)[i] + pose[i]) * .5f; sequence.append(pose);
    }
    for (int i = 0; i < sequence.size(); ++i) {
        original.pose(sequence[i]); candidate.pose(sequence[i]);
        const auto message = difference(frame(original.value), frame(candidate.value));
        if (!message.isEmpty()) return QString("pose %1: %2").arg(i).arg(message);
    }
    for (int i = 0; i < csmGetPartCount(original.value); ++i) {
        csmGetPartOpacities(original.value)[i] = float((i % 3) + 1) / 4;
        csmGetPartOpacities(candidate.value)[i] = csmGetPartOpacities(original.value)[i];
    }
    original.pose(sequence[0]); candidate.pose(sequence[0]);
    const auto parts = difference(frame(original.value), frame(candidate.value));
    if (!parts.isEmpty()) return "nondefault Part opacity: " + parts;
    return original.intact() && candidate.intact() ? QString() : QString("Core allocation canary changed");
}
}
class DeformerStructureReplayCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void rootPreservesActualCore_data()
    {
        QTest::addColumn<QString>("model"); QTest::addColumn<bool>("warp");
        for (const QString name : {"Mark", "Rice", "Natori", "Wanko", "Mao"}) for (bool warp : {false, true}) QTest::newRow(qPrintable(name + (warp ? "-warp" : "-rotation"))) << name << warp;
    }
    void rootPreservesActualCore()
    {
        QFETCH(QString, model); QFETCH(bool, warp); const auto base = Fixture::read(model);
        if (base.isEmpty() && model == "Mao") QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao.moc3");
        const auto insertion = Fixture::rootMesh(base, warp); QVERIFY(!insertion.children.isEmpty());
        RawBuilder::Result added, removed; QString error;
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), added, &error), qPrintable(error));
        error = compare(base, added.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CDeformerStructureReplay::Build(added.bytes, Fixture::removeRequest(added.bytes, insertion.id), removed, &error), qPrintable(error));
        error = compare(base, removed.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void matchingWarpRetainsNonlinearParent_data()
    {
        QTest::addColumn<QString>("model"); for (const QString name : {"Mark", "Rice", "Natori", "Wanko", "Mao"}) QTest::newRow(qPrintable(name)) << name;
    }
    void matchingWarpRetainsNonlinearParent()
    {
        QFETCH(QString, model); const auto base = Fixture::read(model);
        if (base.isEmpty() && model == "Mao") QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao.moc3");
        auto reader = CReader::FromBytes(base); CSectionTable table(reader); RawBuilder::InsertRequest request;
        for (const auto* node : table.deformers) if (node->types == 0) {
            request = Fixture::underParent(base, node->ids, true); if (!request.children.isEmpty()) break;
        }
        QVERIFY(!request.children.isEmpty()); QString error; RawBuilder::Result added, removed;
        CDeformerStructureReplay::Impact impact;
        QVERIFY(CDeformerStructureReplay::Analyze(base, Fixture::insertRequest(base, request), impact, &error));
        QVERIFY(impact.coordinateOwners.isEmpty()); QVERIFY(impact.scalarOwners.isEmpty());
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, request), added, &error), qPrintable(error));
        error = compare(base, added.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CDeformerStructureReplay::Build(added.bytes, Fixture::removeRequest(added.bytes, request.id), removed, &error), qPrintable(error));
        error = compare(base, removed.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void signedRotationParent_data()
    {
        QTest::addColumn<float>("scale"); QTest::addColumn<int>("flags"); QTest::addColumn<bool>("remove");
        for (float scale : {-1.f, .5f, 1.f}) for (int flags = 0; flags < 4; ++flags) for (bool remove : {false, true})
            if (remove || ((scale < 0) == bool(flags & 2))) QTest::newRow(qPrintable(QString("%1-%2-%3").arg(scale).arg(flags).arg(remove))) << scale << flags << remove;
    }
    void signedRotationParent()
    {
        QFETCH(float, scale); QFETCH(int, flags); QFETCH(bool, remove);
        const auto base = Fixture::fixedRotation(Fixture::read("Mark"), "Rotation7", scale, flags);
        QVERIFY(!base.isEmpty()); RawBuilder::Result result; QString error;
        if (remove) QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::removeRequest(base, "Rotation7"), result, &error), qPrintable(error));
        else {
            const auto insertion = Fixture::underParent(base, "Rotation7", false); QVERIFY(!insertion.children.isEmpty());
            QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), result, &error), qPrintable(error));
        }
        error = compare(base, result.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void transformedRotationForSmallWitness_data()
    {
        QTest::addColumn<QString>("parentType"); QTest::addColumn<float>("scale"); QTest::addColumn<int>("flags");
        for (const QString parent : {"root", "rotation", "warp"}) for (float scale : {-2.f, 2.f}) for (int flags : {0, 3})
            QTest::newRow(qPrintable(QString("%1-%2-%3").arg(parent).arg(scale).arg(flags))) << parent << scale << flags;
    }
    void transformedRotationForSmallWitness()
    {
        QFETCH(QString, parentType); QFETCH(float, scale); QFETCH(int, flags);
        auto base = Fixture::fixedRotation(Fixture::read("Mark"), "Rotation7", -.5f, 1, 17);
        QString parentId;
        if (parentType == "warp") {
            RawBuilder::InsertRequest empty; empty.id = "AffineParent"; empty.shape = RawBuilder::Warp{1,1,false, {-.5f,-.25f,1.5f,-.25f,-.5f,.75f,1.5f,.75f}};
            RawBuilder::Result added; QString error; QVERIFY2(RawBuilder::Insert(base, empty, added, &error), qPrintable(error)); base = added.bytes; parentId = empty.id;
        } else if (parentType == "rotation") parentId = "Rotation7";
        auto reader = CReader::FromBytes(base); CSectionTable table(reader); Fixture::RawColumns raw(base);
        const int mesh = 0; const auto* source = table.art_meshes[mesh]; int parent = -1, part = -1;
        for (int i = 0; i < table.deformers.size(); ++i) if (table.deformers[i]->ids == parentId) { parent = i; part = table.deformers[i]->parent_part_indices; }
        raw.set("art_meshes.parent_deformer_indices", mesh, parent); raw.set("art_meshes.parent_part_indices", mesh, part);
        for (int k = 0; k < source->keyform_sources_counts; ++k) {
            const int begin = table.art_mesh_keyforms[source->keyform_sources_begin_indices + k]->keyform_position_sources_begin_indices;
            for (int v = 0; v < source->vertex_counts; ++v) {
                raw.setFloat("keyform_positions.xys", begin + v * 2, float((v % 5) - 2) / 16 + float(k % 3) / 32);
                raw.setFloat("keyform_positions.xys", begin + v * 2 + 1, float((v % 7) - 3) / 16);
            }
        }
        base = raw.encode(); QVERIFY(!base.isEmpty());
        RawBuilder::InsertRequest insertion; insertion.id = "AngledWrapper"; insertion.parentId = parentId; insertion.partIndex = part;
        insertion.children = {{RawBuilder::Owner::ArtMesh, source->ids}};
        insertion.shape = RawBuilder::Rotation{.125f, -.25f, 30, 15, scale, bool(flags & 1), bool(flags & 2)};
        RawBuilder::Result added, removed; QString error;
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), added, &error), qPrintable(error));
        error = compare(base, added.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CDeformerStructureReplay::Build(added.bytes, Fixture::removeRequest(added.bytes, insertion.id), removed, &error), qPrintable(error));
        error = compare(base, removed.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void removalCarriesScaleAcrossWarpOnlyBranches()
    {
        auto base = Fixture::fixedRotation(Fixture::read("Mark"), "Rotation7", .5f, 1);
        auto reader = CReader::FromBytes(base); CSectionTable table(reader); Fixture::RawColumns raw(base);
        int parent = -1, warp = -1;
        for (int i = 0; i < table.deformers.size(); ++i) { if (table.deformers[i]->ids == "Rotation7") parent = i; if (table.deformers[i]->ids == "Warp18") warp = i; }
        QVERIFY(parent >= 0 && warp >= 0); raw.set("deformers.parent_deformer_indices", warp, parent); base = raw.encode();
        QByteArray ordered; QString error; QVERIFY(CMocDeformerOrder::Normalize(base, ordered, &error)); base = ordered;
        CDeformerStructureReplay::Impact impact;
        QVERIFY(CDeformerStructureReplay::Analyze(base, Fixture::removeRequest(base, "Rotation7"), impact, &error));
        QVERIFY(impact.scalarOwners.size() > 1);
        RawBuilder::Result result; QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::removeRequest(base, "Rotation7"), result, &error), qPrintable(error));
        error = compare(base, result.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void halfTurnInsertionRetainsDocumentedRoundoff_data()
    {
        QTest::addColumn<float>("scale"); QTest::addColumn<int>("flags");
        for (float scale : {-1.f, .5f, 1.f}) for (int flags = 0; flags < 4; ++flags) if ((scale < 0) != bool(flags & 2))
            QTest::newRow(qPrintable(QString("%1-%2").arg(scale).arg(flags))) << scale << flags;
    }
    void halfTurnInsertionRetainsDocumentedRoundoff()
    {
        QFETCH(float, scale); QFETCH(int, flags);
        const auto base = Fixture::fixedRotation(Fixture::read("Mark"), "Rotation7", scale, flags);
        const auto insertion = Fixture::underParent(base, "Rotation7", false); QString error;
        CDeformerStructureReplay::Impact impact; QVERIFY(CDeformerStructureReplay::Analyze(base, Fixture::insertRequest(base, insertion), impact, &error));
        QVERIFY(!impact.numericalLimitations.isEmpty()); RawBuilder::Result result;
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), result, &error), qPrintable(error));
        QByteArray normalized; QVERIFY(CMocDeformerOrder::Normalize(result.bytes, normalized, &error));
        CoreModel original, candidate; QVERIFY(original.load(base)); QVERIFY(candidate.load(normalized));
        int strictFailures = 0; double maximumPositionError = 0;
        for (const auto& pose : poses(original.value)) {
            original.pose(pose); candidate.pose(pose); const auto a = frame(original.value), b = frame(candidate.value);
            QCOMPARE(a.definition, b.definition);
            if (!difference(a, b).isEmpty()) ++strictFailures;
            for (int mesh = 0; mesh < a.values.size(); ++mesh) for (int n = 0; n < a.values[mesh].size(); ++n) {
                const double delta = std::abs(double(a.values[mesh][n]) - b.values[mesh][n]);
                if (n < a.positionFields[mesh]) maximumPositionError = std::max(maximumPositionError, delta);
                else QCOMPARE(a.values[mesh][n], b.values[mesh][n]);
            }
        }
        QCOMPARE(strictFailures, poses(original.value).size());
        QVERIFY(maximumPositionError > 0);
        QVERIFY(maximumPositionError <= std::ldexp(std::abs(double(scale)), -15));
        qInfo() << "Documented half-turn float residual; strict failing poses:" << strictFailures << "maximum model distance:" << maximumPositionError;
        QVERIFY(original.intact()); QVERIFY(candidate.intact());
    }
    void warpBlendShapeRowsFollowClonedSpecificSource()
    {
        auto base = Fixture::read("Mao"); if (base.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao.moc3");
        auto reader = CReader::FromBytes(base); CSectionTable table(reader);
        if (table.blend_shapes_warp_deformers.empty()) QSKIP("Official model has no Warp BlendShape target");
        const int specific = table.blend_shapes_warp_deformers.front()->target_indices;
        int target = -1; for (int i = 0; i < table.deformers.size(); ++i) if (table.deformers[i]->types == 0 && table.deformers[i]->specific_sources_indices == specific) { target = i; break; }
        QVERIFY(target >= 0); Fixture::RawColumns raw(base); raw.set("deformers.parent_deformer_indices", target, -1); base = raw.encode();
        QByteArray ordered; QString error; QVERIFY(CMocDeformerOrder::Normalize(base, ordered, &error)); base = ordered;
        RawBuilder::InsertRequest insertion; insertion.id = "WarpBSCage"; insertion.partIndex = table.deformers[target]->parent_part_indices;
        insertion.children = {{RawBuilder::Owner::Warp, table.deformers[target]->ids}};
        insertion.shape = RawBuilder::Warp{1,1,false, {-1,-1,1,-1,-1,1,1,1}};
        RawBuilder::Result added, removed;
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), added, &error), qPrintable(error));
        error = compare(base, added.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
        QVERIFY2(CDeformerStructureReplay::Build(added.bytes, Fixture::removeRequest(added.bytes, insertion.id), removed, &error), qPrintable(error));
        error = compare(base, removed.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
    void blendShapeDeltaUsesLinearPart()
    {
        auto base = Fixture::read("Mao"); if (base.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to the official Mao.moc3");
        auto reader = CReader::FromBytes(base); CSectionTable table(reader); QVERIFY(!table.blend_shapes_art_meshes.empty());
        const int target = table.blend_shapes_art_meshes.front()->target_indices;
        Fixture::RawColumns raw(base); raw.set("art_meshes.parent_deformer_indices", target, -1); base = raw.encode(); QVERIFY(!base.isEmpty());
        RawBuilder::InsertRequest insertion; insertion.id = "BSCage"; insertion.partIndex = table.art_meshes[target]->parent_part_indices;
        insertion.children = {{RawBuilder::Owner::ArtMesh, table.art_meshes[target]->ids}};
        insertion.shape = RawBuilder::Warp{1, 1, false, {-.5f, -.25f, .5f, -.25f, -.5f, .75f, .5f, .75f}};
        RawBuilder::Result result; QString error;
        QVERIFY2(CDeformerStructureReplay::Build(base, Fixture::insertRequest(base, insertion), result, &error), qPrintable(error));
        error = compare(base, result.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
        RawBuilder::Result restored;
        QVERIFY2(CDeformerStructureReplay::Build(result.bytes, Fixture::removeRequest(result.bytes, insertion.id), restored, &error), qPrintable(error));
        error = compare(base, restored.bytes); QVERIFY2(error.isEmpty(), qPrintable(error));
    }
};
QTEST_GUILESS_MAIN(DeformerStructureReplayCoreTests)
#include "deformer_structure_replay_core_tests.moc"
