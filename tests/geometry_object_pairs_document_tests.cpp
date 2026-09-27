#include "geometry_object_pairs_document_fixture.h"
#include "live2dstruct/cmodelsavefingerprint.h"
#include <QtTest>
#include <algorithm>

using namespace GeometryPairsData;

class GeometryObjectPairsDocumentTests : public QObject
{
    Q_OBJECT
private slots:
    void metadataHistoryAndSavedState()
    {
        CModelDocument document; QString error; QVERIFY2(load(document, false, &error), qPrintable(error));
        QByteArray beforeMoc, beforeSaved; QVERIFY(document.BuildRuntimeMoc(beforeMoc)); QVERIFY(CModelSaveFingerprint::Capture(document, beforeSaved));
        const auto original = document.AuthoringStructureState(); const auto state = paired(document);
        QCOMPARE(state.pairs.size(), 1);
        CModelEdit edit; QVERIFY2(document.MakeGeometryObjectPairsEdit(state, edit, &error), qPrintable(error));
        QCOMPARE(edit.field, CModelEdit::GeometryObjectPairs); QVERIFY(!edit.coalesce);
        QCOMPARE(document.AuthoringStructureState(), original); QVERIFY(document.ExecuteEdit(edit));
        QCOMPARE(document.GeometryObjectPairs(), state); QByteArray afterMoc, afterSaved;
        QVERIFY(document.BuildRuntimeMoc(afterMoc)); QCOMPARE(afterMoc, beforeMoc);
        QVERIFY(CModelSaveFingerprint::Capture(document, afterSaved)); QVERIFY(afterSaved != beforeSaved);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original);
        QVERIFY(CModelSaveFingerprint::Capture(document, afterSaved)); QCOMPARE(afterSaved, beforeSaved);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), state);
        QVERIFY(document.MakeGeometryObjectPairsEdit(state, edit)); QVERIFY(!document.ExecuteEdit(edit));
    }

    void rejectedMetadataDoesNotChangeHistory_data()
    {
        QTest::addColumn<int>("failure");
        for (int index = 0; index < 7; ++index) QTest::newRow(qPrintable(QString::number(index))) << index;
    }
    void rejectedMetadataDoesNotChangeHistory()
    {
        QFETCH(int, failure);
        CModelDocument document; QVERIFY(load(document)); CModelEdit edit; QVERIFY(document.MakeGeometryObjectPairsEdit(paired(document), edit));
        if (failure == 0) edit.before = QVariantMap{};
        if (failure == 1) edit.name = "other";
        if (failure == 2) edit.type = "other";
        if (failure == 3) edit.coalesce = true;
        if (failure == 4) edit.index = 0;
        if (failure == 5) { auto state = paired(document); state.sourceDigest[0] ^= 1; edit.after = QVariant::fromValue(state); }
        if (failure == 6) { auto state = paired(document); state.pairs[0].firstVertexToSecondVertex = {0, 0, 2}; edit.after = QVariant::fromValue(state); }
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), baseline); QCOMPARE(document.Revision(), revision);
        QVERIFY(!document.CanUndoEdit());
    }

    void staleMetadataCommandAndClearAreSafe()
    {
        CModelDocument document; QVERIFY(load(document)); CModelEdit first, second;
        auto state = paired(document); QVERIFY(document.MakeGeometryObjectPairsEdit(state, first));
        state.pairs[0].linePoint = {1, 2}; QVERIFY(document.MakeGeometryObjectPairsEdit(state, second));
        QVERIFY(document.ExecuteEdit(first)); QVERIFY(!document.ExecuteEdit(second));
        QVERIFY(document.MakeGeometryObjectPairsEdit({}, second)); QVERIFY(document.ExecuteEdit(second));
        QVERIFY(document.GeometryObjectPairs().pairs.isEmpty()); QVERIFY(document.UndoEdit()); QCOMPARE(document.GeometryObjectPairs(), paired(document));
        document.Clear(); QCOMPARE(document.GeometryObjectPairs(), Pairs::State{});
    }

    void indexChangesReconcileAndUndoMetadata()
    {
        CModelDocument document; QVERIFY(load(document)); const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state));
        const auto before = document.AuthoringStructureState(); auto indices = document.DrawableIndices("Mesh");
        std::swap(indices[0], indices[1]);
        CModelEdit edit{"Mesh", "artmesh", CModelEdit::MeshIndices, QVariant::fromValue(document.DrawableIndices("Mesh")), QVariant::fromValue(indices), -1, false};
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(document.GeometryObjectPairs().pairs[0].requiresReview);
        const auto reviewed = document.GeometryObjectPairs(); QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), reviewed);
        auto cyclic = document.DrawableIndices("Twin"); std::rotate(cyclic.begin(), cyclic.begin() + 1, cyclic.end());
        QVERIFY(document.SetDrawableIndices("Twin", cyclic)); QVERIFY(document.GeometryObjectPairs().pairs[0].requiresReview);
    }

    void canonicalTriangleOrderDoesNotInvalidatePair()
    {
        CModelDocument document; QVERIFY(load(document)); const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state));
        auto indices = document.DrawableIndices("Mesh"); std::rotate(indices.begin(), indices.begin() + 1, indices.end());
        QVERIFY(document.SetDrawableIndices("Mesh", indices)); QCOMPARE(document.GeometryObjectPairs(), state);
    }

    void geometryAndParentsDoNotChangeIdentity()
    {
        CModelDocument document; QVERIFY(load(document)); const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state));
        auto positions = document.DrawableKeyformPositions("Mesh", 0); positions[0] += 0.2f;
        QVERIFY(document.SetDrawableKeyformPositions("Mesh", 0, positions));
        QVERIFY(document.SetDeformerParentIndex("Child", -1)); QCOMPARE(document.GeometryObjectPairs(), state);
        QVERIFY(Pairs::Validate(state, document));
    }

    void structureCannotCarryUnrelatedPairMetadata_data()
    {
        QTest::addColumn<bool>("mesh"); QTest::newRow("mesh") << true; QTest::newRow("parameter") << false;
    }
    void structureCannotCarryUnrelatedPairMetadata()
    {
        QFETCH(bool, mesh);
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        CModelEdit edit;
        if (mesh) QVERIFY2(document.MakeMeshStructureEdit(reorder(document), edit, &error), qPrintable(error));
        else {
            CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "Unrelated";
            QVERIFY2(document.MakeCreateParameterEdit(request, {}, edit, &error), qPrintable(error));
        }
        auto after = edit.after.toMap(); auto state = after.value("geometryObjectPairs").value<Pairs::State>();
        state.pairs[0].linePoint = {42, -17}; after["geometryObjectPairs"] = QVariant::fromValue(state); edit.after = after;
        const auto baseline = document.AuthoringStructureState(); const auto revision = document.Revision();
        QVERIFY(!document.ExecuteEdit(edit)); QCOMPARE(document.AuthoringStructureState(), baseline);
        QCOMPARE(document.Revision(), revision); QVERIFY(!document.CanUndoEdit());
    }

    void topologyHistoryRejectsLaterPairMetadataChange()
    {
        CModelDocument document; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        auto indices = document.DrawableIndices("Mesh"); std::swap(indices[0], indices[1]);
        CModelEdit edit{"Mesh", "artmesh", CModelEdit::MeshIndices, QVariant::fromValue(document.DrawableIndices("Mesh")), QVariant::fromValue(indices), -1, false};
        QVERIFY(document.ExecuteEdit(edit)); auto changed = document.GeometryObjectPairs(); changed.pairs[0].linePoint = {2, 3};
        QVERIFY(document.SetGeometryObjectPairs(changed)); const auto baseline = document.AuthoringStructureState();
        QVERIFY(!document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), baseline); QVERIFY(document.CanUndoEdit());
    }

    void vertexReorderMigratesExplicitIdentity_data()
    {
        QTest::addColumn<bool>("sameTopology");
        QTest::newRow("different-signature") << false; QTest::newRow("same-signature") << true;
    }
    void vertexReorderMigratesExplicitIdentity()
    {
        QFETCH(bool, sameTopology);
        CModelDocument document; QString error; QVERIFY(load(document));
        const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state));
        const auto before = document.AuthoringStructureState(); CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(reorder(document, sameTopology), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); const auto after = document.GeometryObjectPairs();
        QVERIFY(!after.pairs[0].requiresReview);
        const QVector<int> expected = sameTopology ? QVector<int>{1, 2, 0} : QVector<int>{1, 0, 2};
        QCOMPARE(after.pairs[0].firstVertexToSecondVertex, expected);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), after);
    }

    void insertedVertexNeedsExplicitNewPairing()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = "Mesh";
        for (int index = 0; index < 3; ++index) { mesh.vertices.append({{index}, {1}}); mesh.oldToNew.append(index); }
        mesh.vertices.append({{0, 1, 2}, {1.0 / 3, 1.0 / 3, 1.0 / 3}}); mesh.indices = {0, 1, 3, 1, 2, 3, 2, 0, 3};
        CMocMeshStructureBuilder::Request request; request.meshes.append(mesh); CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(request, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.GeometryObjectPairs().pairs[0].requiresReview);
        QVERIFY(document.UndoEdit()); QVERIFY(!document.GeometryObjectPairs().pairs[0].requiresReview);
    }

    void matchingVertexDeletionOnBothSidesKeepsCorrespondence()
    {
        CModelDocument document; QString error; QVERIFY(load(document));
        CMocMeshStructureBuilder::Request insert;
        for (const auto& id : {QString("Mesh"), QString("Twin")}) {
            CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = id;
            for (int index = 0; index < 3; ++index) { mesh.vertices.append({{index}, {1}}); mesh.oldToNew.append(index); }
            mesh.vertices.append({{0, 1, 2}, {1.0 / 3, 1.0 / 3, 1.0 / 3}}); mesh.indices = {0, 1, 3, 1, 2, 3, 2, 0, 3};
            insert.meshes.append(std::move(mesh));
        }
        CModelEdit edit; QVERIFY2(document.MakeMeshStructureEdit(insert, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        const auto before = document.GeometryObjectPairs(); QCOMPARE(before.pairs[0].first.vertexCount, 4);
        CMocMeshStructureBuilder::Request remove;
        for (const auto& id : {QString("Mesh"), QString("Twin")}) {
            CMocMeshStructureBuilder::MeshEdit mesh; mesh.artMeshId = id;
            for (int index = 0; index < 3; ++index) mesh.vertices.append({{index}, {1}});
            mesh.oldToNew = {0, 1, 2, -1}; mesh.indices = {0, 1, 2}; remove.meshes.append(std::move(mesh));
        }
        QVERIFY2(document.MakeMeshStructureEdit(remove, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        const auto after = document.GeometryObjectPairs(); QVERIFY(!after.pairs[0].requiresReview);
        QCOMPARE(after.pairs[0].first.vertexCount, 3); QCOMPARE(after.pairs[0].firstVertexToSecondVertex, QVector<int>({0, 1, 2}));
        QVERIFY(document.UndoEdit()); QCOMPARE(document.GeometryObjectPairs(), before);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), after);
    }

    void bindingKeyIdentityMigratesRatherThanNearestValues_data()
    {
        QTest::addColumn<int>("change");
        QTest::newRow("move") << 0; QTest::newRow("same-values-different-identities") << 1; QTest::newRow("insert") << 2;
    }
    void bindingKeyIdentityMigratesRatherThanNearestValues()
    {
        QFETCH(int, change);
        CModelDocument document; QString error; QVERIFY(load(document));
        const auto state = paired(document); QVERIFY(document.SetGeometryObjectPairs(state));
        const auto original = document.AuthoringStructureState();
        CMocParameterStructureBuilder::EditBindingKeysRequest request;
        request.owner = {CMocParameterStructureBuilder::Owner::ArtMesh, "Mesh"}; request.axisIndex = 0;
        request.parameterId = "P"; request.expectedKeys = {0, 1};
        if (change == 0) request.keys = {{0.2f, 0}, {0.8f, 1}};
        if (change == 1) request.keys = {{0, 1}, {1, 0}};
        if (change == 2) request.keys = {{0, 0}, {0.5f, -1}, {1, 1}};
        CModelEdit edit; QVERIFY2(document.MakeBindingKeysEdit(request, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); const auto migrated = document.GeometryObjectPairs();
        QCOMPARE(migrated.pairs[0].requiresReview, change == 2);
        if (change < 2) {
            const QVector<int> expected = change == 1 ? QVector<int>{1, 0} : QVector<int>{0, 1};
            QCOMPARE(migrated.pairs[0].axes[0].firstKeyToSecondKey, expected);
        }
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), migrated);
    }

    void reviewPairDoesNotBlockUnrelatedParameterCreation()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        auto indices = document.DrawableIndices("Mesh"); std::swap(indices[0], indices[1]); QVERIFY(document.SetDrawableIndices("Mesh", indices));
        const auto reviewed = document.GeometryObjectPairs(); QVERIFY(reviewed.pairs[0].requiresReview);
        CMocParameterStructureBuilder::CreateRequest request; request.parameter.id = "Unrelated";
        CModelEdit edit; QVERIFY2(document.MakeCreateParameterEdit(request, {}, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QCOMPARE(document.GeometryObjectPairs(), reviewed);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.GeometryObjectPairs(), reviewed);
    }

    void deletingSharedParameterRemovesItsAxisOnBothSides()
    {
        CModelDocument document; QString error; QVERIFY(load(document)); QVERIFY(document.SetGeometryObjectPairs(paired(document)));
        const auto original = document.AuthoringStructureState(); CModelEdit edit;
        QVERIFY2(document.MakeDeleteParameterEdit(CMocParameterStructureBuilder::DeleteRequest{"P", 0.5f}, edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); const auto state = document.GeometryObjectPairs();
        QCOMPARE(state.pairs.size(), 1); QVERIFY(!state.pairs[0].requiresReview); QVERIFY(state.pairs[0].axes.isEmpty());
        QCOMPARE(state.pairs[0].first.keyformCount, 1); QCOMPARE(state.pairs[0].second.keyformCount, 1);
        QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original);
        QVERIFY(document.RedoEdit()); QCOMPARE(document.GeometryObjectPairs(), state);
    }

    void deletingAndRecreatingObjectDoesNotReviveItsPair()
    {
        CModelDocument document; QString error; QVERIFY2(load(document, true, &error), qPrintable(error));
        QVERIFY(document.SetGeometryObjectPairs(paired(document, true))); const auto original = document.AuthoringStructureState();
        CModelEdit edit; QVERIFY2(document.MakeDeformerStructureEdit(removal(document, "Twin"), edit, &error), qPrintable(error));
        QVERIFY(document.ExecuteEdit(edit)); QVERIFY(document.GeometryObjectPairs().pairs.isEmpty());
        CMocDeformerStructureBuilder::InsertRequest insertion; insertion.id = "Twin"; insertion.shape = CMocDeformerStructureBuilder::Rotation{};
        QByteArray bytes; QVERIFY(document.BuildPreviewMoc(bytes));
        CDeformerPreserveRequest request{insertion, document.SourceDigest(), QCryptographicHash::hash(bytes, QCryptographicHash::Sha256)};
        QVERIFY2(document.MakeDeformerStructureEdit(request, edit, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(edit));
        QVERIFY(document.GeometryObjectPairs().pairs.isEmpty());
        QVERIFY(document.UndoEdit()); QVERIFY(document.UndoEdit()); QCOMPARE(document.AuthoringStructureState(), original);
    }
};

QTEST_APPLESS_MAIN(GeometryObjectPairsDocumentTests)
#include "geometry_object_pairs_document_tests.moc"
