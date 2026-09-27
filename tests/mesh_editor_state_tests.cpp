#include "edit/cmeshmap.h"

#include <QtTest>
#include <limits>

namespace {
const float sourcePositions[]{-0.5f, -0.5f, 0.5f, -0.5f, 0.0f, 0.5f};
const float candidatePositions[]{-0.8f, -0.2f, 0.2f, -0.2f, -0.3f, 0.8f};
const float candidateUvs[]{0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f};
const short sourceIndices[]{0, 1, 2};
const short candidateIndices[]{2, 1, 0};

QVector<bool> selected(const CDrawMesh& mesh)
{
    QVector<bool> result;
    for (const auto* point : mesh.GetMesh()->GetWolrd()) result.append(point->GetIsSelect());
    return result;
}
}

class MeshEditorStateTests : public QObject
{
    Q_OBJECT
private slots:
    void paintedStateCopiesWithoutChangingHardSelectionOrGeometry()
    {
        CDrawMesh source(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh target(3, candidateIndices, 6, candidateUvs, candidatePositions);
        CDrawMesh::EditorState state;
        QVERIFY(source.CaptureEditorState(state));
        state.paintedWeights = QVector<double>{0, .25, .875};
        state.shown = true;
        QVERIFY(source.RestoreEditorState(state));
        QVERIFY(target.CopyEditorStateFrom(source));
        CDrawMesh::EditorState copied;
        QVERIFY(target.CaptureEditorState(copied));
        QCOMPARE(*copied.paintedWeights, *state.paintedWeights);
        QVERIFY(copied.selectedVertices.isEmpty());
        QVERIFY(target.GetSelectPoint().empty());
        QCOMPARE(target.Positions(), QVector<float>(candidatePositions, candidatePositions + 6));
        state.paintedWeights->fill(1);
        QVERIFY(target.CaptureEditorState(copied));
        QCOMPARE(*copied.paintedWeights, (QVector<double>{0, .25, .875}));
    }

    void paintedMigrationMapsIdentityAndZerosInsertedPoints()
    {
        const float next[]{0, 0, 1, 0, 1, 1, 0, 1};
        CDrawMesh sourceMesh(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh targetMesh(3, sourceIndices, 8, next, next);
        CDrawMesh::EditorState state;
        QVERIFY(sourceMesh.CaptureEditorState(state));
        state.paintedWeights = QVector<double>{.125, .5, .875};
        QVERIFY(sourceMesh.RestoreEditorState(state));
        CMeshMap source, target;
        source.insert("A", &sourceMesh); target.insert("A", &targetMesh);
        CMeshGeometryMigration migration;
        migration.remaps.insert("A", {3, 4, {2, -1, 0}});
        QVERIFY(target.CopyEditorStateFrom(source, migration));
        QVERIFY(targetMesh.CaptureEditorState(state));
        QCOMPARE(*state.paintedWeights, (QVector<double>{.875, 0, .125, 0}));
        QVERIFY(state.selectedVertices.isEmpty());
    }

    void invalidPaintedCandidateDoesNotPartiallyCommitOtherMeshes()
    {
        CDrawMesh a(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh b(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CMeshMap map; map.insert("A", &a); map.insert("B", &b);
        CMeshEditorSnapshot baseline; QVERIFY(map.CaptureEditorState(baseline));
        for (const auto& invalid : QVector<QVector<double>>{{0, 1}, {0, -1, 0}, {0, 1.01, 0},
                 {0, std::numeric_limits<double>::quiet_NaN(), 0}}) {
            auto candidate = baseline;
            candidate["A"].paintedWeights = QVector<double>{.1, .2, .3};
            candidate["B"].paintedWeights = invalid;
            QVERIFY(!map.RestoreEditorState(candidate));
            CMeshEditorSnapshot after; QVERIFY(map.CaptureEditorState(after));
            QVERIFY(!after["A"].paintedWeights); QVERIFY(!after["B"].paintedWeights);
        }
        auto conflict = baseline;
        conflict["B"].paintedWeights = QVector<double>{0, 0, 0};
        conflict["B"].selectedVertices = {1};
        QVERIFY(!map.RestoreEditorState(conflict));
    }

    void legacyInsertionPreservesPaintAndStartsNewPointAtZero()
    {
        CDrawMesh mesh(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh::EditorState state; QVERIFY(mesh.CaptureEditorState(state));
        state.paintedWeights = QVector<double>{.25, .5, .75};
        QVERIFY(mesh.RestoreEditorState(state));
        mesh.AddPoint({0, 0});
        QVERIFY(mesh.CaptureEditorState(state));
        QCOMPARE(state.vertexCount, 4);
        QCOMPARE(*state.paintedWeights, (QVector<double>{.25, .5, .75, 0}));
        mesh.AddPoint({100, 100});
        QVERIFY(mesh.CaptureEditorState(state));
        QCOMPARE(state.vertexCount, 4);
        QCOMPARE(*state.paintedWeights, (QVector<double>{.25, .5, .75, 0}));
    }

    void explicitZeroPaintPersistsAndAcceptedHardSelectionClearsIt()
    {
        CDrawMesh mesh(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh::EditorState state; QVERIFY(mesh.CaptureEditorState(state));
        state.paintedWeights = QVector<double>(3, 0);
        QVERIFY(mesh.RestoreEditorState(state));
        QVERIFY(mesh.CaptureEditorState(state)); QVERIFY(state.paintedWeights);
        QVERIFY(mesh.SelectNearestPoint({0, .5f}, .01f));
        QVERIFY(mesh.CaptureEditorState(state)); QVERIFY(!state.paintedWeights);
        QCOMPARE(state.selectedVertices, QVector<int>{2});
        state.selectedVertices.clear(); state.paintedWeights = QVector<double>(3, .25);
        QVERIFY(mesh.RestoreEditorState(state)); mesh.CancelSelect();
        QVERIFY(mesh.CaptureEditorState(state)); QVERIFY(!state.paintedWeights);
    }

    void migrationUsesCandidatePointsAndPreservesBothGeometries()
    {
        CDrawMesh sourceA(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh sourceB(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh candidateA(3, candidateIndices, 6, candidateUvs, candidatePositions);
        CDrawMesh candidateB(3, candidateIndices, 6, candidateUvs, candidatePositions);
        sourceA.SetShow(true);
        sourceA.SetIsOver(true);
        sourceA.SetSelect(QRectF(-0.6, -0.6, 1.2, 0.2));
        sourceB.SetIsOver(true);
        QVERIFY(sourceB.SelectNearestPoint({0, 0.5f}, 0.01f));
        candidateB.SetShow(true);
        QVERIFY(candidateA.SelectNearestPoint({-0.3f, 0.8f}, 0.01f));
        CMeshMap source, candidate;
        source.insert("A", &sourceA);
        source.insert("B", &sourceB);
        candidate.insert("B", &candidateB);
        candidate.insert("A", &candidateA);
        const auto sourceBefore = sourceA.Positions();
        const auto candidateBefore = candidateA.Positions();
        QVERIFY(candidate.CopyEditorStateFrom(source));
        QVERIFY(candidateA.IsShown());
        QVERIFY(candidateA.IsHovered());
        QVERIFY(!candidateB.IsShown());
        QVERIFY(candidateB.IsHovered());
        QCOMPARE(selected(candidateA), (QVector<bool>{true, true, false}));
        QCOMPARE(selected(candidateB), (QVector<bool>{false, false, true}));
        QCOMPARE(candidateA.GetSelectPoint().size(), size_t(2));
        for (size_t index = 0; index < candidateA.GetSelectPoint().size(); ++index) {
            QCOMPARE(candidateA.GetSelectPoint()[index], candidateA.GetMesh()->GetWolrd()[index]);
            QVERIFY(candidateA.GetSelectPoint()[index] != sourceA.GetSelectPoint()[index]);
        }
        QCOMPARE(candidateB.GetSelectPoint().front(), candidateB.GetMesh()->GetWolrd()[2]);
        QCOMPARE(candidateA.Positions(), candidateBefore);
        QCOMPARE(sourceA.Positions(), sourceBefore);
        for (int index = 0; index < 3; ++index) {
            QCOMPARE(candidateA.GetMesh()->GetEBOIndex()[index], candidateIndices[index]);
            QCOMPARE(sourceA.GetMesh()->GetEBOIndex()[index], sourceIndices[index]);
        }
        for (int index = 0; index < 6; ++index)
            QCOMPARE(candidateA.GetMesh()->GetUV()[index], candidateUvs[index]);
        candidateA.MoveSelected({0.1f, 0.2f});
        const auto moved = candidateA.Positions();
        for (int index = 0; index < 3; ++index) {
            QCOMPARE(moved[index * 2], candidateBefore[index * 2] + (index < 2 ? 0.1f : 0));
            QCOMPARE(moved[index * 2 + 1], candidateBefore[index * 2 + 1] + (index < 2 ? 0.2f : 0));
        }
        QCOMPARE(sourceA.Positions(), sourceBefore);
        sourceA.CancelSelect();
        QCOMPARE(selected(candidateA), (QVector<bool>{true, true, false}));
        QVERIFY(candidate.CopyEditorStateFrom(candidate));
        QCOMPARE(candidateA.Positions(), moved);
        QCOMPARE(selected(candidateA), (QVector<bool>{true, true, false}));
    }

    void topologyCountChangesKeepPointSelection_data()
    {
        QTest::addColumn<int>("sourceCount"); QTest::addColumn<int>("candidateCount");
        for (int source : {0, 3, 6}) for (int target : {0, 3, 6})
            QTest::newRow(qPrintable(QStringLiteral("%1-to-%2").arg(source).arg(target))) << source << target;
    }

    void topologyCountChangesKeepPointSelection()
    {
        QFETCH(int, sourceCount); QFETCH(int, candidateCount);
        const short indices[]{0, 1, 2, 2, 1, 0};
        CDrawMesh sourceMesh(sourceCount, sourceCount ? indices : nullptr, 6, sourcePositions, sourcePositions);
        CDrawMesh targetMesh(candidateCount, candidateCount ? indices : nullptr, 6, candidateUvs, candidatePositions);
        sourceMesh.SetShow(true); sourceMesh.SetIsOver(true);
        sourceMesh.SetSelect(QRectF(-0.6, -0.6, 1.2, 0.2));
        CMeshMap source, target; source.insert("mesh", &sourceMesh); target.insert("mesh", &targetMesh);
        const auto positions = targetMesh.Positions();
        QVERIFY(target.CopyEditorStateFrom(source));
        QCOMPARE(targetMesh.GetMesh()->GetEBOSize(), candidateCount);
        QCOMPARE(targetMesh.Positions(), positions);
        QCOMPARE(targetMesh.GetMesh()->GetWolrd().size(), size_t(3));
        QCOMPARE(targetMesh.GetMesh()->GetEdge().size(), size_t(candidateCount));
        QCOMPARE(selected(targetMesh), (QVector<bool>{true, true, false}));
        QVERIFY(targetMesh.IsShown()); QVERIFY(targetMesh.IsHovered());
        for (size_t index = 0; index < targetMesh.GetSelectPoint().size(); ++index) {
            QCOMPARE(targetMesh.GetSelectPoint()[index], targetMesh.GetMesh()->GetWolrd()[index]);
            QVERIFY(targetMesh.GetSelectPoint()[index] != sourceMesh.GetSelectPoint()[index]);
        }
        targetMesh.MoveSelected({0.1f, 0});
        QCOMPARE(sourceMesh.Positions(), QVector<float>({-0.5f, -0.5f, 0.5f, -0.5f, 0, 0.5f}));
    }

    void invalidMapsDoNotPartiallyReplaceEarlierObjects_data()
    {
        QTest::addColumn<QString>("invalid");
        for (const char* invalid : {"missing-id", "different-id", "null-source", "null-target", "vertex-count"})
            QTest::newRow(invalid) << QString::fromLatin1(invalid);
    }

    void invalidMapsDoNotPartiallyReplaceEarlierObjects()
    {
        QFETCH(QString, invalid);
        CDrawMesh sourceA(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh sourceZ(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh candidateA(3, candidateIndices, 6, candidateUvs, candidatePositions);
        CDrawMesh candidateZ(3, candidateIndices, 6, candidateUvs, candidatePositions);
        const float quad[]{-0.5f, -0.5f, 0.5f, -0.5f, 0.5f, 0.5f, -0.5f, 0.5f};
        CDrawMesh differentVertices(3, sourceIndices, 8, quad, quad);
        sourceA.SetShow(true);
        sourceA.SetIsOver(true);
        sourceA.SetSelect(QRectF(-0.6, -0.6, 1.2, 0.2));
        QVERIFY(candidateA.SelectNearestPoint({-0.3f, 0.8f}, 0.01f));
        CMeshMap source, candidate;
        source.insert("A", &sourceA);
        source.insert("Z", invalid == "null-source" ? nullptr : &sourceZ);
        candidate.insert("A", &candidateA);
        if (invalid != "missing-id") {
            candidate.insert(invalid == "different-id" ? "other" : "Z",
                invalid == "null-target" ? nullptr : invalid == "vertex-count" ? &differentVertices : &candidateZ);
        }
        const auto positions = candidateA.Positions();
        const auto flags = selected(candidateA);
        const auto pointers = candidateA.GetSelectPoint();
        QVERIFY(!candidate.CopyEditorStateFrom(source));
        QVERIFY(!candidateA.IsShown());
        QVERIFY(!candidateA.IsHovered());
        QCOMPARE(candidateA.Positions(), positions);
        QCOMPARE(selected(candidateA), flags);
        QVERIFY(candidateA.GetSelectPoint() == pointers);
        QVERIFY(sourceA.IsShown());
        QVERIFY(sourceA.IsHovered());
        QCOMPARE(selected(sourceA), (QVector<bool>{true, true, false}));
    }

    void geometryRemapPreservesIdentityAndRestoresDeletedSelection()
    {
        const float quad[]{-1, -1, 1, -1, 1, 1, -1, 1};
        const short triangles[]{0, 1, 2, 0, 2, 3};
        CDrawMesh sourceMesh(6, triangles, 8, quad, quad);
        CDrawMesh removedMesh(3, sourceIndices, 6, candidateUvs, candidatePositions);
        CDrawMesh restoredMesh(6, triangles, 8, quad, quad);
        sourceMesh.SetShow(true); sourceMesh.SetIsOver(true);
        sourceMesh.GetMesh()->GetWolrd()[0]->SetIsSelect(true);
        sourceMesh.GetMesh()->GetWolrd()[3]->SetIsSelect(true);
        CMeshMap source, removed, restored;
        source.insert("mesh", &sourceMesh); removed.insert("mesh", &removedMesh); restored.insert("mesh", &restoredMesh);
        CMeshEditorSnapshot original;
        QVERIFY(source.CaptureEditorState(original));
        const auto stored = QVariant::fromValue(original).value<CMeshEditorSnapshot>();
        QCOMPARE(stored.value("mesh").selectedVertices, QVector<int>({0, 3}));
        CMeshGeometryMigration removal;
        removal.remaps.insert("mesh", {4, 3, {2, 0, 1, -1}});
        QVERIFY(removed.CopyEditorStateFrom(source, removal));
        QCOMPARE(selected(removedMesh), QVector<bool>({false, false, true}));
        QCOMPARE(removedMesh.GetSelectPoint().front(), removedMesh.GetMesh()->GetWolrd()[2]);
        QVERIFY(removedMesh.IsShown()); QVERIFY(removedMesh.IsHovered());
        CMeshGeometryMigration undo;
        undo.remaps.insert("mesh", {3, 4, {1, 2, 0}});
        undo.targetState = stored;
        QVERIFY(restored.CopyEditorStateFrom(removed, undo));
        QCOMPARE(selected(restoredMesh), QVector<bool>({true, false, false, true}));
        QVERIFY(restoredMesh.GetSelectPoint().front() != sourceMesh.GetSelectPoint().front());
        QCOMPARE(sourceMesh.Positions(), QVector<float>(quad, quad + 8));
        QCOMPARE(removedMesh.Positions(), QVector<float>(candidatePositions, candidatePositions + 6));
    }

    void equalCountReorderingAndNewPointsUseExplicitMapping()
    {
        CDrawMesh sourceMesh(3, sourceIndices, 6, sourcePositions, sourcePositions);
        CDrawMesh reorderedMesh(3, candidateIndices, 6, candidateUvs, candidatePositions);
        const float quad[]{-1,-1,1,-1,1,1,-1,1};
        CDrawMesh grownMesh(3, sourceIndices, 8, quad, quad);
        sourceMesh.GetMesh()->GetWolrd()[1]->SetIsSelect(true);
        CMeshMap source, reordered, grown;
        source.insert("mesh", &sourceMesh); reordered.insert("mesh", &reorderedMesh); grown.insert("mesh", &grownMesh);
        CMeshGeometryMigration reorder; reorder.remaps.insert("mesh", {3,3,{1,2,0}});
        QVERIFY(reordered.CopyEditorStateFrom(source, reorder));
        QCOMPARE(selected(reorderedMesh), QVector<bool>({false,false,true}));
        CMeshGeometryMigration grow; grow.remaps.insert("mesh", {3,4,{2,0,3}});
        QVERIFY(grown.CopyEditorStateFrom(source, grow));
        QCOMPARE(selected(grownMesh), QVector<bool>({true,false,false,false}));
    }

    void invalidGeometryMigrationIsAtomic_data()
    {
        QTest::addColumn<int>("invalid");
        for (int i=0; i<8; ++i) QTest::newRow(qPrintable(QString::number(i))) << i;
    }
    void invalidGeometryMigrationIsAtomic()
    {
        QFETCH(int, invalid);
        CDrawMesh sourceA(3,sourceIndices,6,sourcePositions,sourcePositions), sourceZ(3,sourceIndices,6,sourcePositions,sourcePositions);
        CDrawMesh targetA(3,candidateIndices,6,candidateUvs,candidatePositions), targetZ(3,candidateIndices,6,candidateUvs,candidatePositions);
        sourceA.SetShow(true); sourceA.GetMesh()->GetWolrd()[0]->SetIsSelect(true);
        targetA.GetMesh()->GetWolrd()[2]->SetIsSelect(true);
        CMeshMap source, target; source.insert("A",&sourceA);source.insert("Z",&sourceZ);target.insert("A",&targetA);target.insert("Z",&targetZ);
        CMeshGeometryMigration migration; migration.remaps.insert("A",{3,3,{2,0,1}});migration.remaps.insert("Z",{3,3,{0,1,2}});
        switch(invalid) {
        case 0:migration.remaps["Z"].oldToNew={0,0,2};break;
        case 1:migration.remaps["Z"].oldToNew={0,1,3};break;
        case 2:migration.remaps["Z"].oldToNew={0,1,-2};break;
        case 3:migration.remaps["Z"].oldVertexCount=4;break;
        case 4:migration.remaps["Z"].newVertexCount=4;break;
        case 5:migration.remaps.insert("unknown",{3,3,{0,1,2}});break;
        default:{ CMeshEditorSnapshot state; QVERIFY(source.CaptureEditorState(state));
            if(invalid==6) state["Z"].selectedVertices={0,0};else state.remove("Z");
            migration.targetState=state;break; }
        }
        QString error; QVERIFY(!target.CopyEditorStateFrom(source,migration,&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(selected(targetA),QVector<bool>({false,false,true}));QVERIFY(!targetA.IsShown());
        QCOMPARE(selected(sourceA),QVector<bool>({true,false,false}));QVERIFY(sourceA.IsShown());
    }

    void everySelectionPathExposesTheCurrentPointFlags()
    {
        CDrawMesh mesh(3, sourceIndices, 6, sourcePositions, sourcePositions);
        mesh.SetSelect(QRectF(-0.6, -0.6, 1.2, 0.2));
        QCOMPARE(mesh.GetSelectPoint().size(), size_t(2));
        QVERIFY(mesh.SelectNearestPoint({0, 0.5f}, 0.01f));
        QCOMPARE(mesh.GetSelectPoint().size(), size_t(1));
        QCOMPARE(mesh.GetSelectPoint().front(), mesh.GetMesh()->GetWolrd()[2]);
        QVERIFY(!mesh.SelectNearestPoint({2, 2}, 0.01f));
        QVERIFY(mesh.GetSelectPoint().empty());
        QVERIFY(mesh.GetMesh()->SelectNearestPoint({-0.5f, -0.5f}, 0.01f));
        QCOMPARE(mesh.GetSelectPoint().front(), mesh.GetMesh()->GetWolrd()[0]);
        QVERIFY(!mesh.SelectNearestPoint({std::numeric_limits<float>::infinity(), 0}, 1));
        QCOMPARE(mesh.GetSelectPoint().front(), mesh.GetMesh()->GetWolrd()[0]);
        mesh.CancelSelect();
        QVERIFY(mesh.GetSelectPoint().empty());
        QCOMPARE(selected(mesh), (QVector<bool>{false, false, false}));
    }

    void missingLookupDoesNotInsertMeshEntries()
    {
        CMeshMap source, candidate;
        QCOMPARE(source.GetDrawMesh("missing"), nullptr);
        QVERIFY(source.GetMapMesh().isEmpty());
        const CMeshMap& readOnly = source;
        QCOMPARE(readOnly.GetDrawMesh("missing"), nullptr);
        QVERIFY(readOnly.GetMapMesh().isEmpty());
        QVERIFY(candidate.CopyEditorStateFrom(source));
    }
};

QTEST_GUILESS_MAIN(MeshEditorStateTests)
#include "mesh_editor_state_tests.moc"
