#include "geometry_pair_planner_fixture.h"
#include "live2dstruct/cdeformereditcontext.h"
#include "geometry_object_pairs_document_fixture.h"
#include <QtTest>

using namespace GeometryPairData;

class GeometryPairPlannerTests : public QObject
{
    Q_OBJECT
private slots:
    void mirrorUsesIndependentPosesAndOneGeometryCommand_data()
    {
        QTest::addColumn<int>("type");
        for (int type : {0, 1, 2}) QTest::newRow(qPrintable(QString::number(type))) << type;
    }
    void mirrorUsesIndependentPosesAndOneGeometryCommand()
    {
        QFETCH(int, type); Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Type(type), fixture, &error, true, true), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "matrix.moc3", &error));
        auto pair = fixture.pair;
        if (type == Geometry::Rotation)
            QVERIFY2(CGeometryPairPlanner::PrepareRotationCorrespondence(document, pair, 0, 3, {}, pair, &error), qPrintable(error));
        CGeometryPairPlanner::Result result;
        const auto revision = document.Revision(); const auto state = document.AuthoringStructureState();
        QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, pair, true, 0, {}, result, &error), qPrintable(error));
        QVERIFY(result.changed); QCOMPARE(result.sourcePose.value("PairPrimary"), -1.0f);
        QCOMPARE(result.targetPose.value("PairPrimary"), 1.0f);
        QCOMPARE(result.edit.field, CModelEdit::GeometryKeyforms);
        const auto before = result.edit.before.value<CModelGeometryState>();
        const auto after = result.edit.after.value<CModelGeometryState>();
        QCOMPARE(before.size(), 2); QCOMPARE(after.size(), 2); QCOMPARE(before[0], after[0]);
        QCOMPARE(before[0].keyform, 0); QCOMPARE(before[1].keyform, 3);
        QCOMPARE(document.Revision(), revision); QCOMPARE(document.AuthoringStructureState(), state);
        QVERIFY(document.ExecuteEdit(result.edit)); QVERIFY(document.UndoEdit());
        QVERIFY(!document.CanUndoEdit()); QVERIFY(document.CanRedoEdit());
    }

    void synchronizedNoopKeepsRedoAndPreciseValues_data() { mirrorUsesIndependentPosesAndOneGeometryCommand_data(); }
    void synchronizedNoopKeepsRedoAndPreciseValues()
    {
        QFETCH(int, type); Fixture fixture; QString error;
        QVERIFY2(make(Geometry::Type(type), fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "noop.moc3", &error));
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::Type(type), fixture.pair.first.object.id, 0, source));
        CModelEdit prior; QVERIFY(document.MakeGeometryEdit({source}, {changed(source)}, source, prior));
        QVERIFY(document.ExecuteEdit(prior)); QVERIFY(document.UndoEdit()); QVERIFY(document.CanRedoEdit());
        const auto revision = document.Revision(); const auto state = document.AuthoringStructureState();
        CGeometryPairPlanner::Result result;
        QVERIFY2(CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, source, source, {}, result, &error), qPrintable(error));
        QVERIFY(!result.changed); QVERIFY(document.CanRedoEdit());
        QCOMPARE(document.Revision(), revision); QCOMPARE(document.AuthoringStructureState(), state);
    }

    void failuresPreserveResultAndDocument()
    {
        Fixture fixture; QString error; QVERIFY2(make(Geometry::ArtMesh, fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "failure.moc3"));
        CGeometryPairPlanner::Result result; result.changed = true; result.sourceModel = {7, 8}; result.diagnostics = {"held"};
        const auto revision = document.Revision(); const auto state = document.AuthoringStructureState();
        auto invalid = fixture.pair; invalid.firstVertexToSecondVertex[0] = invalid.firstVertexToSecondVertex[1];
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, invalid, true, 0, {}, result, &error));
        QCOMPARE(result.sourceModel, QVector<float>({7, 8})); QCOMPARE(result.diagnostics, QStringList({"held"}));
        invalid = fixture.pair; invalid.requiresReview = true;
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, invalid, true, 0, {}, result, &error));
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {{"missing", 0}}, result, &error));
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::ArtMesh, fixture.pair.first.object.id, 0, source));
        auto bad = source; std::get<QVector<float>>(bad.value)[0] = std::numeric_limits<float>::quiet_NaN();
        QVERIFY(!CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, source, bad, {}, result, &error));
        QVERIFY(!CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, changed(source), source, {}, result, &error));
        QCOMPARE(document.Revision(), revision); QCOMPARE(document.AuthoringStructureState(), state);
        QCOMPARE(result.sourceModel, QVector<float>({7, 8})); QCOMPARE(result.diagnostics, QStringList({"held"}));
    }

    void culledTriangleCannotSilentlyFlip()
    {
        Fixture fixture; QString error;
        QVERIFY2(make(Geometry::ArtMesh, fixture, &error, false, false, false, true), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "culling.moc3"));
        CGeometryPairPlanner::Result result;
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, result, &error));
        QVERIFY2(error.contains(QStringLiteral("剔除")), qPrintable(error));
        const auto target = fixture.pair.second.object.id;
        auto collapsed = document.DrawableKeyformPositions(target, 0); std::fill(collapsed.begin(), collapsed.end(), 0);
        QVERIFY(document.SetDrawableKeyformPositions(target, 0, collapsed));
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, result, &error));
        QVERIFY2(error.contains(QStringLiteral("反向绕序")), qPrintable(error));
        QVERIFY2(make(Geometry::ArtMesh, fixture, &error, false, false, true, true), qPrintable(error));
        QVERIFY(document.LoadBytes(fixture.bytes, "reversed.moc3"));
        QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, result, &error), qPrintable(error));
        QVERIFY(result.changed);
    }

    void zeroScaleRequiresExplicitCopyWhenExpanding()
    {
        Fixture fixture; QString error; QVERIFY2(make(Geometry::Rotation, fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "zero.moc3"));
        auto forms = document.DeformerRotationKeyforms(fixture.pair.first.object.id); forms[0].scale = 0;
        QVERIFY(document.SetDeformerRotationKeyforms(fixture.pair.first.object.id, forms));
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::Rotation, fixture.pair.first.object.id, 0, source));
        auto expanded = source; std::get<CModelRotationTransform>(expanded.value).scale = 1;
        CGeometryPairPlanner::Result result;
        QVERIFY(!CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, source, expanded, {}, result, &error));
        QVERIFY2(error.contains(QStringLiteral("镜像复制")), qPrintable(error));
        auto rotated = source; std::get<CModelRotationTransform>(rotated.value).angle += 10;
        QVERIFY2(CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, source, rotated, {}, result, &error), qPrintable(error));
        QVERIFY(result.changed);
    }

    void multiKeyRotationPreservesReflectionFlags()
    {
        Fixture fixture; QString error; QVERIFY2(make(Geometry::Rotation, fixture, &error, false, true), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "reflections.moc3"));
        CGeometryPairPlanner::Result result;
        QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, result, &error));
        QVERIFY2(error.contains(QStringLiteral("反射")), qPrintable(error));
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::Rotation, fixture.pair.first.object.id, 0, source));
        auto changedReflection = source;
        auto& transform = std::get<CModelRotationTransform>(changedReflection.value);
        transform.reflectX = !transform.reflectX; transform.reflectY = !transform.reflectY;
        QVERIFY(!CGeometryPairPlanner::PrepareSynchronized(document, fixture.pair, true, source, changedReflection, {}, result, &error));
        QVERIFY2(error.contains(QStringLiteral("反射")), qPrintable(error));
        auto pair = fixture.pair;
        QVERIFY(CGeometryPairPlanner::PrepareRotationCorrespondence(document, pair, 0, 3, {}, pair, &error));
        QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, pair, true, 0, {}, result, &error), qPrintable(error));
        const auto original = document.DeformerRotationKeyforms(pair.second.object.id)[3];
        if (result.changed) {
            const auto after = result.edit.after.value<CModelGeometryState>();
            const auto transform = std::get<CModelRotationTransform>(after[1].value);
            QCOMPARE(transform.reflectX, original.reflectX); QCOMPARE(transform.reflectY, original.reflectY);
        }
    }

    void cachedContextUsesFrozenStartWithoutAccumulating()
    {
        Fixture fixture; QString error; QVERIFY2(make(Geometry::Warp, fixture, &error), qPrintable(error));
        CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "cached.moc3"));
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::Warp, fixture.pair.first.object.id, 0, source));
        CGeometryPairPlanner::SynchronizationContext context;
        QVERIFY2(CGeometryPairPlanner::PrepareSynchronization(document, fixture.pair, true, 0, {}, context, &error), qPrintable(error));
        QVERIFY(context.IsValid());
        auto first = changed(source), second = first; std::get<QVector<float>>(second.value)[0] += .025f;
        CGeometryPairPlanner::Result initial, advanced, repeated, restored;
        QVERIFY(CGeometryPairPlanner::Synchronize(context, first, initial, &error));
        QVERIFY(CGeometryPairPlanner::Synchronize(context, second, advanced, &error));
        QVERIFY(document.ExecuteEdit(advanced.edit));
        QVERIFY(CGeometryPairPlanner::Synchronize(context, first, repeated, &error));
        QCOMPARE(initial.edit.before.value<CModelGeometryState>(), repeated.edit.before.value<CModelGeometryState>());
        QCOMPARE(initial.edit.after.value<CModelGeometryState>(), repeated.edit.after.value<CModelGeometryState>());
        QCOMPARE(initial.targetModel, repeated.targetModel); QCOMPARE(initial.targetBeforeModel, repeated.targetBeforeModel);
        QVERIFY(CGeometryPairPlanner::Synchronize(context, source, restored, &error)); QVERIFY(!restored.changed);
        QVERIFY(!document.ExecuteEdit(initial.edit));
        auto invalid = fixture.pair; invalid.enabled = false;
        QVERIFY(!CGeometryPairPlanner::PrepareSynchronization(document, invalid, true, 0, {}, context, &error));
        QVERIFY(context.IsValid());
        QVERIFY(CGeometryPairPlanner::Synchronize(context, first, repeated, &error));
        QCOMPARE(initial.targetModel, repeated.targetModel);
    }

    void sharedSpecificAcceptsEqualWritesAndRejectsDivergence()
    {
        CModelDocument document; QString error;
        QVERIFY2(GeometryPairsData::load(document, true, &error), qPrintable(error));
        const auto pairs = GeometryPairsData::paired(document, true); QVERIFY(pairs.pairs.size() == 1);
        Geometry source; QVERIFY(document.CaptureGeometryKeyform(Geometry::Rotation, "Root", 0, source));
        auto after = source; std::get<CModelRotationTransform>(after.value).originY += .25f;
        CGeometryPairPlanner::SynchronizationContext context;
        QVERIFY2(CGeometryPairPlanner::PrepareSynchronization(document, pairs.pairs[0], true, 0, {}, context, &error), qPrintable(error));
        CGeometryPairPlanner::Result result;
        QVERIFY2(CGeometryPairPlanner::Synchronize(context, after, result, &error), qPrintable(error));
        QVERIFY(result.changed);
        const auto values = result.edit.after.value<CModelGeometryState>();
        QCOMPARE(std::get<CModelRotationTransform>(values[0].value), std::get<CModelRotationTransform>(values[1].value));
        after = source; std::get<CModelRotationTransform>(after.value).originX += .25f;
        const auto held = result.targetModel;
        QVERIFY(!CGeometryPairPlanner::Synchronize(context, after, result, &error));
        QVERIFY2(error.contains(QStringLiteral("共享")), qPrintable(error)); QCOMPARE(result.targetModel, held);
    }

    void ancestorPairsAreRejectedBeforePlanning()
    {
        for (const auto type : {Geometry::Warp, Geometry::Rotation}) {
            Fixture fixture; QString error; QVERIFY2(make(type, fixture, &error), qPrintable(error));
            CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "ancestor.moc3"));
            int parent = -1;
            for (int index = 0; index < document.Deformers().size(); ++index)
                if (document.Deformers()[index].id == fixture.pair.first.object.id) parent = index;
            QVERIFY(parent >= 0); QVERIFY(document.SetDeformerParentIndices({{fixture.pair.second.object.id, parent}}));
            CGeometryPairPlanner::Result result; result.targetModel = {7, 9};
            QVERIFY(!CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, result, &error));
            QVERIFY2(error.contains(QStringLiteral("祖先")), qPrintable(error));
            QCOMPARE(result.targetModel, QVector<float>({7, 9}));
        }
    }

    void arbitraryLineUsesModelSpaceForEveryGeometryType()
    {
        for (const auto type : {Geometry::ArtMesh, Geometry::Warp, Geometry::Rotation}) {
            Fixture fixture; QString error; QVERIFY2(make(type, fixture, &error), qPrintable(error));
            CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "diagonal.moc3"));
            auto pair = fixture.pair; pair.linePoint = {.2, -.3}; pair.lineDirection = {1, 1};
            CGeometryPairPlanner::Result result;
            QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, pair, true, 0, {}, result, &error), qPrintable(error));
            QCOMPARE(result.sourceModel.size(), result.targetModel.size());
            for (int vertex = 0; vertex < result.sourceModel.size() / 2; ++vertex) {
                QVERIFY(std::abs(result.targetModel[vertex * 2] - (result.sourceModel[vertex * 2 + 1] + .5)) < .00002);
                QVERIFY(std::abs(result.targetModel[vertex * 2 + 1] - (result.sourceModel[vertex * 2] - .5)) < .00002);
            }
            QVERIFY(result.targetBeforeModel != result.targetModel);
        }
    }

    void repeatedMirrorDoesNotCreateHistoryOrLoseRedo()
    {
        for (const auto type : {Geometry::ArtMesh, Geometry::Warp, Geometry::Rotation}) {
            Fixture fixture; QString error; QVERIFY2(make(type, fixture, &error), qPrintable(error));
            CModelDocument document; QVERIFY(document.LoadBytes(fixture.bytes, "repeat.moc3"));
            CGeometryPairPlanner::Result first, repeated;
            QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, first, &error), qPrintable(error));
            QVERIFY(first.changed); QVERIFY(document.ExecuteEdit(first.edit));
            Geometry source; QVERIFY(document.CaptureGeometryKeyform(type, fixture.pair.first.object.id, 0, source));
            CModelEdit unrelated; QVERIFY(document.MakeGeometryEdit({source}, {changed(source)}, source, unrelated));
            QVERIFY(document.ExecuteEdit(unrelated)); QVERIFY(document.UndoEdit()); QVERIFY(document.CanRedoEdit());
            const auto revision = document.Revision();
            QVERIFY2(CGeometryPairPlanner::PrepareMirror(document, fixture.pair, true, 0, {}, repeated, &error), qPrintable(error));
            QVERIFY(!repeated.changed); QVERIFY(document.CanRedoEdit()); QCOMPARE(document.Revision(), revision);
        }
    }
};

QTEST_GUILESS_MAIN(GeometryPairPlannerTests)
#include "geometry_pair_planner_tests.moc"
