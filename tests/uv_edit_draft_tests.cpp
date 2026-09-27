#include "live2dstruct/cuveditdraft.h"
#include <QtTest>
#include <cmath>
#include <limits>

using namespace live2dstruct;
namespace {
CUvEditDraft::Snapshot snapshot()
{
    return {QByteArray(32, 'a'), 7, QByteArray(32, 'b'),
        {{{"A", 0, {.1f,.2f,.3f,.2f,.1f,.4f,.7f,.7f,.9f,.7f,.7f,.9f}}, {0,1,2,3,4,5}, true, {}},
         {{"B", 1, {0,0,1,0,0,1}}, {0,1,2}, false, "locked"}}, {{0, QSize(100,200)}, {1, QSize(200,100)}}};
}
}
class UvEditDraftTests : public QObject
{
    Q_OBJECT
private slots:
    void initializeRejectsMalformedSnapshotWithoutReplacingAccepted()
    {
        CUvEditDraft draft; auto source = snapshot(); QString error; QVERIFY(draft.Initialize(source, &error));
        const auto initial = draft.InitialState();
        for (int kind = 0; kind < 7; ++kind) {
            auto invalid = source;
            if (kind == 0) invalid.meshes[1].material.id = "A";
            if (kind == 1) invalid.meshes[0].material.uvs.append(1);
            if (kind == 2) invalid.meshes[0].material.textureIndex = 3;
            if (kind == 3) invalid.meshes[0].triangles[0] = 20;
            if (kind == 4) invalid.meshes[0].triangles.append(0);
            if (kind == 5) invalid.meshes[0].material.uvs[0] = std::numeric_limits<float>::infinity();
            if (kind == 6) invalid.atlasSizes[0] = {};
            QVERIFY(!draft.Initialize(invalid, &error)); QVERIFY(!error.isEmpty()); QCOMPARE(draft.InitialState(), initial);
        }
    }
    void pixelTranslationKeepsUnselectedVerticesAndOtherObjectsExact()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState();
        CModelMaterialState result; CUvEditDraft::Transform transform; transform.translation = {5, -10};
        QVERIFY(draft.TransformSelection(initial, {"A", {0,2}}, transform, result));
        QVERIFY(std::abs(result[0].uvs[0] - .15f) < 1e-7f); QVERIFY(std::abs(result[0].uvs[1] - .15f) < 1e-7f);
        QCOMPARE(result[0].uvs[2], initial[0].uvs[2]); QCOMPARE(result[1], initial[1]);
        QRectF bounds; QVERIFY(draft.SelectionBounds(result, {"A", {0,2}}, bounds)); QVERIFY(std::abs(bounds.left()-15) < 1e-5);
    }
    void rotationAndIndependentScaleUsePixelSpace()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState();
        CModelMaterialState result; CUvEditDraft::Transform transform; transform.pivot={10,40}; transform.scale={2,.5}; transform.angleDegrees=90;
        QVERIFY(draft.TransformSelection(initial, {"A", {1}}, transform, result));
        QVERIFY(std::abs(result[0].uvs[2]-.1f)<1e-6f); QVERIFY(std::abs(result[0].uvs[3]-.4f)<1e-6f);
    }
    void normalizedAndPixelTextureTransferAreDistinct()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState(); CModelMaterialState result;
        QVERIFY(draft.AssignTexture(initial,"A",1,CUvEditDraft::SlotCoordinates::Normalized,result)); QCOMPARE(result[0].uvs,initial[0].uvs); QCOMPARE(result[0].textureIndex,1);
        QVERIFY(draft.AssignTexture(initial,"A",1,CUvEditDraft::SlotCoordinates::Pixels,result)); QCOMPARE(result[0].uvs[0],initial[0].uvs[0]/2); QCOMPARE(result[0].uvs[1],initial[0].uvs[1]*2);
        QCOMPARE(result[1],initial[1]);
    }
    void finiteOutsideAndDegenerateGeometryRemainEditableAndDiagnosed()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); auto initial = draft.InitialState(); CModelMaterialState result;
        CUvEditDraft::Transform transform; transform.translation={-500,0}; transform.scale={0,0};
        QVERIFY(draft.TransformSelection(initial,{"A",{0,1,2}},transform,result)); QVERIFY(result[0].uvs[0]<0);
        const auto diagnostics=draft.Diagnostics(result,"A"); QCOMPARE(diagnostics.size(),2); QVERIFY(diagnostics[0].contains("3"));
    }
    void invalidRequestPreservesOutput()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState(); CModelMaterialState result=initial; QString error;
        CUvEditDraft::Transform transform; transform.translation={std::numeric_limits<double>::infinity(),0};
        QVERIFY(!draft.TransformSelection(initial,{"A",{0}},transform,result,&error)); QCOMPARE(result,initial);
        transform.translation={1e100,0}; QVERIFY(!draft.TransformSelection(initial,{"A",{0}},transform,result,&error)); QCOMPARE(result,initial);
        QVERIFY(!draft.SetPointPixels(initial,{"A",{0,1}},{2,3},result,&error)); QCOMPARE(result,initial);
        QVERIFY(!draft.TransformSelection(initial,{"A",{500}}, {},result,&error)); QCOMPARE(result,initial);
    }
    void lockedTargetsCannotChangeThroughAnyOperator()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState(); CModelMaterialState result=initial;
        QVERIFY(!draft.TransformSelection(initial,{"B",{0}}, {},result)); QVERIFY(!draft.AssignTexture(initial,"B",0,CUvEditDraft::SlotCoordinates::Normalized,result));
        auto illegal=initial; illegal[1].uvs[0]=.5f; QVERIFY(!draft.ValidateState(illegal)); QCOMPARE(result,initial);
    }
    void resetSelectedKeepsSlotAndFullResetRestoresAllEditableMaterials()
    {
        CUvEditDraft draft; QVERIFY(draft.Initialize(snapshot())); const auto initial = draft.InitialState(); CModelMaterialState changed,result;
        QVERIFY(draft.AssignTexture(initial,"A",1,CUvEditDraft::SlotCoordinates::Pixels,changed));
        QVERIFY(draft.Reset(changed,{"A",{0}},false,result)); QCOMPARE(result[0].textureIndex,1); QCOMPARE(result[0].uvs[0],initial[0].uvs[0]); QCOMPARE(result[0].uvs[2],changed[0].uvs[2]);
        QVERIFY(draft.Reset(changed,{},true,result)); QCOMPARE(result,initial);
    }
    void islandsFollowVertexConnectivityRatherThanOverlappingCoordinates()
    {
        CUvEditDraft draft; auto source=snapshot(); source.meshes[0].material.uvs={0,0,1,0,0,1,0,0,1,0,0,1}; QVERIFY(draft.Initialize(source));
        QCOMPARE(draft.ConnectedIsland("A",0),QSet<int>({0,1,2})); QCOMPARE(draft.ConnectedIsland("A",3),QSet<int>({3,4,5})); QVERIFY(draft.ConnectedIsland("missing",0).isEmpty());
    }
    void snappingIsExplicitAndNoopPreservesFloatValues()
    {
        CUvEditDraft draft; auto source=snapshot(); source.meshes[0].material.uvs[0]=.12345678f; QVERIFY(draft.Initialize(source));
        const auto initial=draft.InitialState(); CModelMaterialState result; QVERIFY(draft.TransformSelection(initial,{"A",{0}}, {},result)); QCOMPARE(result,initial);
        CUvEditDraft::Transform transform; transform.snapToPixels=true; QVERIFY(draft.TransformSelection(initial,{"A",{0}},transform,result)); QCOMPARE(result[0].uvs[0],.12f);
    }
    void identityAndLockedValidationPreserveSignedZeroBits()
    {
        auto source=snapshot(); source.meshes[0].material.uvs[0]=-0.0f; source.meshes[1].material.uvs[0]=-0.0f;
        CUvEditDraft draft; QVERIFY(draft.Initialize(source)); const auto original=draft.InitialState(); CModelMaterialState result;
        QVERIFY(draft.TransformSelection(original,{"A",{0}}, {},result)); QCOMPARE(result,original); QVERIFY(std::signbit(result[0].uvs[0]));
        auto changed=original; changed[1].uvs[0]=0.0f; QVERIFY(!draft.ValidateState(changed));
    }
};
QTEST_GUILESS_MAIN(UvEditDraftTests)
#include "uv_edit_draft_tests.moc"
