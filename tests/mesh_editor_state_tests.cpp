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
