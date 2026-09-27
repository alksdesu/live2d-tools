#include "framework/CubismFramework.hpp"
#include "framework/ICubismAllocator.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismModelReplacement.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include "wide_mesh_fixture.h"
#include <QtTest>
#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>
#include <cstdlib>
#include <map>
#include <new>
#include <stdexcept>

using namespace Live2D::Cubism::Framework;
using Rendering::CubismRenderer;
using Builder = live2dstruct::CMocMeshStructureBuilder;

namespace {
class Allocator final : public ICubismAllocator
{
public:
    void* Allocate(csmSizeType size) override { auto* p=std::malloc(size);if(p)plain.insert(p);return p; }
    void Deallocate(void* p) override { plain.remove(p);std::free(p); }
    void* AllocateAligned(csmSizeType size,csmUint32 alignment) override
    { auto* p=::operator new(size,std::align_val_t(alignment),std::nothrow);if(p)aligned.emplace(p,alignment);return p; }
    void DeallocateAligned(void* p) override
    { if(!p)return;const auto it=aligned.find(p);if(it==aligned.end())qFatal("Unknown Core allocation");::operator delete(p,std::align_val_t(it->second));aligned.erase(it); }
    QSet<void*> plain;
    std::map<void*,unsigned> aligned;
};
class BoundaryRenderer final : public CubismRenderer
{
    csmBool DoDrawModel() override { qFatal("This real Core test does not render");return false; }
    csmBool SaveProfile() override { qFatal("This real Core test does not enter GL");return false; }
    csmBool RestoreProfile() override { return false; }
};
struct ModelOwner {
    CubismMoc* moc=nullptr;
    CubismModel* model=nullptr;
    CubismRenderer* renderer=nullptr;
    ~ModelOwner() { CubismRenderer::Delete(renderer);if(moc&&model)moc->DeleteModel(model);CubismMoc::Delete(moc); }
    bool Load(const QByteArray& bytes) {
        moc=CubismMoc::Create(reinterpret_cast<const csmByte*>(bytes.constData()),bytes.size(),true);
        if(!moc||!(model=moc->CreateModel()))return false;
        model->Update();model->InitMesh();renderer=CubismRenderer::Create();return renderer&&renderer->Initialize(model);
    }
};
QByteArray read(const QString& path) { QFile f(path);return f.open(QIODevice::ReadOnly)?f.readAll():QByteArray(); }
Builder::MeshEdit identity(const live2dstruct::CSectionTable& table,int index) {
    const auto* mesh=table.art_meshes[index];Builder::MeshEdit edit;edit.artMeshId=mesh->ids;
    for(int v=0;v<mesh->vertex_counts;++v){edit.vertices.append({{v},{1}});edit.oldToNew.append(v);}
    for(int i=0;i<mesh->position_index_sources_counts;++i)edit.indices.append(quint16(table.position_indices->indices[mesh->position_index_sources_begin_indices+i]));
    return edit;
}
CMeshGeometryMigration migration(const Builder::MeshRemap& remap) {
    CMeshGeometryMigration result;result.remaps.insert(remap.artMeshId,{remap.oldVertexCount,remap.newVertexCount,remap.oldToNew});return result;
}
CubismModelReplacement::RendererPreparation prepareRenderer() {
    return [](CubismRenderer* candidate,CubismModel* model,const CubismRenderer& previous,QString*) {
        const auto color=previous.GetModelColor();candidate->SetModelColor(color.R,color.G,color.B,color.A);return candidate->Initialize(model);
    };
}
}
namespace Live2D { namespace Cubism { namespace Framework { namespace Rendering {
CubismRenderer* CubismRenderer::Create() { return CSM_NEW BoundaryRenderer; }
void CubismRenderer::StaticRelease() {}
}}}}

class MeshGeometryReplacementCoreTests : public QObject
{
    Q_OBJECT
    Allocator allocator;
private slots:
    void initTestCase() {
        QVERIFY(CubismFramework::StartUp(&allocator));CubismFramework::Initialize();
    }
    void cleanupTestCase() {
        CubismFramework::Dispose();CubismFramework::CleanUp();QVERIFY(allocator.plain.isEmpty());QVERIFY(allocator.aligned.empty());
    }
    void unsignedSourcesKeepTheirEditorDirectoryAndOverrides_data()
    {
        QTest::addColumn<int>("count"); QTest::addColumn<bool>("grid");
        QTest::newRow("high-bit") << 32769 << false;
        QTest::newRow("full-index-grid") << 65536 << true;
        QTest::newRow("unreferenced-source-tail") << 65537 << false;
    }
    void unsignedSourcesKeepTheirEditorDirectoryAndOverrides()
    {
        QFETCH(int, count); QFETCH(bool, grid);
        WideMeshData::Fixture fixture; QString error;
        QVERIFY2(WideMeshData::make(count, grid ? WideMeshData::Layout::Grid : WideMeshData::Layout::Sparse, fixture, &error), qPrintable(error));
        ModelOwner source; QVERIFY(source.Load(fixture.bytes));
        const auto name = fixture.ids.first();
        const int drawable = source.model->GetDrawableIndex(CubismFramework::GetIdManager()->GetId(name.toUtf8().constData()));
        QVERIFY(drawable >= 0); QCOMPARE(source.model->GetDrawableVertexCount(drawable), count);
        auto* mesh = source.model->GetMeshMap()->GetDrawMesh(name); QVERIFY(mesh);
        QCOMPARE(mesh->GetMesh()->GetWolrd().size(), size_t(count));
        const int indexCount = source.model->GetDrawableVertexIndexCount(drawable);
        QCOMPARE(indexCount, fixture.indices.size()); if (grid) QVERIFY(indexCount > 65536);
        const auto* raw = source.model->GetDrawableVertexIndices(drawable);
        QVector<quint16> indices(raw, raw + indexCount);
        QVERIFY(indices.contains(quint16(count >= 65536 ? 65535 : 32768)));
        QCOMPARE(QVector<quint16>(mesh->GetMesh()->GetEBOIndex(), mesh->GetMesh()->GetEBOIndex() + mesh->GetMesh()->GetEBOSize()), indices);
        std::swap(indices[0], indices[1]);
        QVERIFY(source.model->SetDrawableIndicesOverride(drawable, indices));
        CDrawMesh::EditorState state; QVERIFY(mesh->CaptureEditorState(state));
        state.shown = true; state.paintedWeights = QVector<double>(count, 0);
        (*state.paintedWeights)[count >= 65536 ? 65535 : 32768] = .375;
        QVERIFY(mesh->RestoreEditorState(state));
        source.model->SaveParameters();
        source.model->SetParameterValue(0, source.model->GetParameterMaximumValue(0));
        source.model->SetParameterValue(CubismFramework::GetIdManager()->GetId("WideVirtual"), .625f);
        source.model->SetModelOpacity(.75f); source.model->Update();
        CubismModel::RuntimeValueSnapshot before; QVERIFY(source.model->CaptureRuntimeValues(before));
        CubismModelReplacement rejected;
        const auto reject = [](CubismRenderer*, CubismModel*, const CubismRenderer&, QString* error) { *error = "controlled rejection"; return false; };
        QVERIFY(!rejected.Prepare(reinterpret_cast<const csmByte*>(fixture.bytes.constData()), fixture.bytes.size(),
            *source.moc, *source.model, *source.renderer, reject, &error));
        QCOMPARE(source.model->GetMeshMap()->GetDrawMesh(name), mesh);
        CDrawMesh::EditorState held; QVERIFY(mesh->CaptureEditorState(held)); QCOMPARE(held.paintedWeights, state.paintedWeights);
        CubismModelReplacement candidate;
        QVERIFY2(candidate.Prepare(reinterpret_cast<const csmByte*>(fixture.bytes.constData()), fixture.bytes.size(),
            *source.moc, *source.model, *source.renderer, prepareRenderer(), &error), qPrintable(error));
        auto* next = candidate.GetModel();
        CubismModel::RuntimeValueSnapshot after; QVERIFY(next->CaptureRuntimeValues(after));
        QCOMPARE(after.parameterValues, before.parameterValues); QCOMPARE(after.savedParameters, before.savedParameters);
        QCOMPARE(after.savedPresence, before.savedPresence); QCOMPARE(after.partOpacities, before.partOpacities);
        QCOMPARE(after.virtualParameterIds, before.virtualParameterIds); QCOMPARE(after.virtualParameterValues, before.virtualParameterValues);
        QCOMPARE(after.modelOpacity, before.modelOpacity);
        QCOMPARE(QVector<quint16>(next->GetDrawableVertexIndices(drawable), next->GetDrawableVertexIndices(drawable) + indexCount), indices);
        for (int index = 0; index < source.model->GetDrawableCount(); ++index) {
            QCOMPARE(next->GetDrawableVertexCount(index), source.model->GetDrawableVertexCount(index));
            QCOMPARE(next->GetDrawableVertexIndexCount(index), source.model->GetDrawableVertexIndexCount(index));
            const int vertices = source.model->GetDrawableVertexCount(index), triangles = source.model->GetDrawableVertexIndexCount(index);
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(next->GetDrawableVertices(index)), vertices * 2 * sizeof(float)),
                     QByteArray(reinterpret_cast<const char*>(source.model->GetDrawableVertices(index)), vertices * 2 * sizeof(float)));
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(next->GetDrawableVertexUvs(index)), vertices * 2 * sizeof(float)),
                     QByteArray(reinterpret_cast<const char*>(source.model->GetDrawableVertexUvs(index)), vertices * 2 * sizeof(float)));
            QCOMPARE(QByteArray(reinterpret_cast<const char*>(next->GetDrawableVertexIndices(index)), triangles * sizeof(quint16)),
                     QByteArray(reinterpret_cast<const char*>(source.model->GetDrawableVertexIndices(index)), triangles * sizeof(quint16)));
        }
        auto* nextMesh = next->GetMeshMap()->GetDrawMesh(name); QVERIFY(nextMesh);
        CDrawMesh::EditorState restored; QVERIFY(nextMesh->CaptureEditorState(restored));
        QCOMPARE(restored.vertexCount, count); QCOMPARE(restored.paintedWeights, state.paintedWeights); QVERIFY(restored.shown);
        QVERIFY(candidate.Commit(source.moc, source.model, source.renderer));
        QCOMPARE(source.model->GetDrawableVertexIndices(drawable)[0], indices.first());
    }

    void unsignedInsertionAndUndoRetainPaintedVertexIdentity_data()
    {
        QTest::addColumn<int>("count");
        QTest::newRow("cross-signed-boundary") << 32768;
        QTest::newRow("fill-unsigned-capacity") << 65535;
    }
    void unsignedInsertionAndUndoRetainPaintedVertexIdentity()
    {
        QFETCH(int, count); WideMeshData::Fixture fixture; QString error;
        QVERIFY2(WideMeshData::make(count, WideMeshData::Layout::Sparse, fixture, &error), qPrintable(error));
        auto reader = live2dstruct::CReader::FromBytes(fixture.bytes); live2dstruct::CSectionTable table(reader); QVERIFY(table.IsValid());
        int target = -1; for (int i = 0; i < table.art_meshes.size(); ++i) if (table.art_meshes[i]->ids == fixture.ids.first()) target = i;
        QVERIFY(target >= 0); auto edit = identity(table, target);
        const auto a = edit.indices[0], b = edit.indices[1], c = edit.indices[2];
        edit.vertices.append({{a, b, c}, {.5, .25, .25}});
        edit.indices = {a, b, quint16(count), b, c, quint16(count), c, a, quint16(count)};
        Builder::Request request; request.meshes.append(edit); Builder::Result built;
        QVERIFY2(Builder::Build(fixture.bytes, request, built, &error), qPrintable(error));
        ModelOwner source; QVERIFY(source.Load(fixture.bytes));
        const auto name = fixture.ids.first(); const int drawable = source.model->GetDrawableIndex(CubismFramework::GetIdManager()->GetId(name.toUtf8().constData()));
        auto* mesh = source.model->GetMeshMap()->GetDrawMesh(name); QVERIFY(mesh);
        CDrawMesh::EditorState state; QVERIFY(mesh->CaptureEditorState(state));
        state.paintedWeights = QVector<double>(count, 0); (*state.paintedWeights)[count - 1] = .625;
        QVERIFY(mesh->RestoreEditorState(state));
        CMeshEditorSnapshot original; QVERIFY(source.model->GetMeshMap()->CaptureEditorState(original));
        source.model->SaveParameters(); source.model->SetParameterValue(0, source.model->GetParameterMaximumValue(0)); source.model->Update();
        CubismModel::RuntimeValueSnapshot before; QVERIFY(source.model->CaptureRuntimeValues(before));
        auto migrated = migration(built.meshes.first()); CubismModelReplacement next;
        const auto policy = CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        QVERIFY2(next.Prepare(reinterpret_cast<const csmByte*>(built.bytes.constData()), built.bytes.size(), *source.moc, *source.model,
            *source.renderer, prepareRenderer(), policy, {}, &error, {}, &migrated), qPrintable(error));
        QCOMPARE(next.GetModel()->GetDrawableVertexCount(drawable), count + 1);
        QCOMPARE(next.GetModel()->GetDrawableVertexIndexCount(drawable), 9);
        const auto* ebo = next.GetModel()->GetDrawableVertexIndices(drawable);
        QVERIFY(std::find(ebo, ebo + 9, quint16(count)) != ebo + 9);
        CDrawMesh::EditorState actual; QVERIFY(next.GetModel()->GetMeshMap()->GetDrawMesh(name)->CaptureEditorState(actual));
        QCOMPARE(actual.paintedWeights->size(), count + 1); QCOMPARE(actual.paintedWeights->at(count - 1), .625); QCOMPARE(actual.paintedWeights->at(count), 0.);
        CubismModel::RuntimeValueSnapshot after; QVERIFY(next.GetModel()->CaptureRuntimeValues(after));
        QCOMPARE(after.parameterValues, before.parameterValues); QCOMPARE(after.savedParameters, before.savedParameters); QCOMPARE(after.savedPresence, before.savedPresence);
        QVERIFY(next.Commit(source.moc, source.model, source.renderer));
        if (count + 1 == Builder::MaximumVertices) {
            auto parsed = live2dstruct::CReader::FromBytes(built.bytes); live2dstruct::CSectionTable current(parsed);
            auto overflow = identity(current, target); overflow.vertices.append({{0}, {1}});
            Builder::Request invalid; invalid.meshes.append(overflow); Builder::Result sentinel; sentinel.bytes = "keep";
            QVERIFY(!Builder::Build(built.bytes, invalid, sentinel, &error)); QCOMPARE(sentinel.bytes, QByteArray("keep"));
        }
        CMeshGeometryMigration undo; undo.remaps.insert(name, {count + 1, count, built.meshes.first().newToOld}); undo.targetState = original;
        CubismModelReplacement restored;
        QVERIFY2(restored.Prepare(reinterpret_cast<const csmByte*>(fixture.bytes.constData()), fixture.bytes.size(), *source.moc, *source.model,
            *source.renderer, prepareRenderer(), policy, {}, &error, {}, &undo), qPrintable(error));
        QVERIFY(restored.Commit(source.moc, source.model, source.renderer));
        QVERIFY(source.model->GetMeshMap()->GetDrawMesh(name)->CaptureEditorState(actual));
        QCOMPARE(actual.vertexCount, count); QCOMPARE(actual.paintedWeights, state.paintedWeights);
        QVERIFY(source.model->CaptureRuntimeValues(after)); QCOMPARE(after.parameterValues, before.parameterValues); QCOMPARE(after.savedParameters, before.savedParameters);
    }

    void emptyRawDrawableRetainsIdentityAndUndoSelection()
    {
        const auto bytes=read(QString(TEST_RESOURCES)+"/Mark/Mark.moc3");auto reader=live2dstruct::CReader::FromBytes(bytes);live2dstruct::CSectionTable table(reader);QVERIFY(table.IsValid());
        const auto* mesh=table.art_meshes[0];const QString name=mesh->ids;const int count=mesh->vertex_counts;QVERIFY(count>0);
        auto empty=bytes;qToLittleEndian(qint32(0),empty.data()+table.sectionOffsetTable->art_meshes->vertex_counts);
        qToLittleEndian(qint32(0),empty.data()+table.sectionOffsetTable->art_meshes->position_index_sources_counts);
        ModelOwner source;QVERIFY(source.Load(bytes));auto* oldMesh=source.model->GetMeshMap()->GetDrawMesh(name);QVERIFY(oldMesh);
        oldMesh->SetShow(true);oldMesh->GetMesh()->GetWolrd()[count-1]->SetIsSelect(true);CMeshEditorSnapshot original;QVERIFY(source.model->GetMeshMap()->CaptureEditorState(original));
        CMeshGeometryMigration migration;migration.remaps.insert(name,{count,0,QVector<int>(count,-1)});
        CubismModelReplacement candidate;QString error;const auto policy=CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        QVERIFY2(candidate.Prepare(reinterpret_cast<const csmByte*>(empty.constData()),empty.size(),*source.moc,*source.model,*source.renderer,prepareRenderer(),policy,{},&error,{},&migration),qPrintable(error));
        QVERIFY(candidate.Commit(source.moc,source.model,source.renderer));auto* emptyMesh=source.model->GetMeshMap()->GetDrawMesh(name);QVERIFY(emptyMesh);QVERIFY(emptyMesh->IsShown());QVERIFY(emptyMesh->GetMesh()->GetWolrd().empty());
        CMeshGeometryMigration undo;undo.remaps.insert(name,{0,count,{}});undo.targetState=original;CubismModelReplacement restored;
        QVERIFY2(restored.Prepare(reinterpret_cast<const csmByte*>(bytes.constData()),bytes.size(),*source.moc,*source.model,*source.renderer,prepareRenderer(),policy,{},&error,{},&undo),qPrintable(error));
        QVERIFY(restored.Commit(source.moc,source.model,source.renderer));QVERIFY(source.model->GetMeshMap()->GetDrawMesh(name)->GetMesh()->GetWolrd()[count-1]->GetIsSelect());
    }

    void newPointIsCheckedAgainstEvaluatedCoreBeforeRenderer()
    {
        const auto bytes = read(QString(TEST_RESOURCES) + "/Mark/Mark.moc3");
        auto reader = live2dstruct::CReader::FromBytes(bytes);
        live2dstruct::CSectionTable table(reader);
        QVERIFY(table.IsValid());
        auto edit = identity(table, 0);
        const int oldCount = edit.vertices.size();
        QVERIFY(oldCount > 0);
        const int a = edit.indices[0], b = edit.indices[1], c = edit.indices[2];
        edit.vertices.append({{a, b, c}, {0.2, 0.3, 0.5}});
        edit.indices[1] = quint16(oldCount);
        edit.indices += QVector<quint16>{quint16(oldCount), quint16(b), quint16(c)};
        Builder::Request request;
        request.meshes.append(edit);
        Builder::Result built;
        QString error;
        QVERIFY2(Builder::Build(bytes, request, built, &error), qPrintable(error));
        ModelOwner source, independent;
        QVERIFY(source.Load(bytes));
        QVERIFY(independent.Load(built.bytes));
        const auto* positions = independent.model->GetDrawableVertexPositions(0);
        const QPointF expected(positions[oldCount].X, positions[oldCount].Y);
        auto mapping = migration(built.meshes.first());
        mapping.expectation = CMeshGeometryExpectation{{{edit.artMeshId, oldCount, expected}}, 0.000001};
        CubismModelReplacement replacement;
        bool rendererCalled = false;
        const auto render = [&](CubismRenderer* next, CubismModel* model, const CubismRenderer& previous, QString* detail) {
            rendererCalled = true;
            return prepareRenderer()(next, model, previous, detail);
        };
        const auto policy = CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        QVERIFY2(replacement.Prepare(reinterpret_cast<const csmByte*>(built.bytes.constData()), built.bytes.size(),
            *source.moc, *source.model, *source.renderer, render, policy, {}, &error, {}, &mapping), qPrintable(error));
        QVERIFY(rendererCalled);
        QVERIFY(replacement.GetModel());
        mapping.expectation->points[0].modelPosition += QPointF(0.01, 0.02);
        rendererCalled = false;
        CubismModelReplacement rejected;
        const auto allocationCount = allocator.aligned.size();
        auto* old = source.model;
        QVERIFY(!rejected.Prepare(reinterpret_cast<const csmByte*>(built.bytes.constData()), built.bytes.size(),
            *source.moc, *source.model, *source.renderer, render, policy, {}, &error, {}, &mapping));
        QVERIFY(!error.isEmpty());
        QVERIFY(!rendererCalled);
        QVERIFY(!rejected.GetModel());
        QCOMPARE(source.model, old);
        QCOMPARE(source.model->GetDrawableVertexCount(0), oldCount);
        QCOMPARE(allocator.aligned.size(), allocationCount);
    }

    void realCandidatesPreserveRuntimeAndVertexIdentity_data() {
        QTest::addColumn<QString>("path");QTest::addColumn<QString>("operation");
        for(const auto& name:{QString("Mark"),QString("Natori"),QString("Rice"),QString("Wanko")})
            for(const auto& op:{QString("grow"),QString("delete"),QString("reorder")})
                QTest::newRow(qPrintable(name+'-'+op))<<QString(TEST_RESOURCES)+'/'+name+'/'+name+".moc3"<<op;
        const auto extra=qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if(!extra.isEmpty())for(const auto& op:{QString("grow"),QString("delete"),QString("reorder")})QTest::newRow(qPrintable("Mao-"+op))<<extra<<op;
    }
    void realCandidatesPreserveRuntimeAndVertexIdentity() {
        QFETCH(QString,path);QFETCH(QString,operation);const auto bytes=read(path);QVERIFY(!bytes.isEmpty());const auto digest=QCryptographicHash::hash(bytes,QCryptographicHash::Sha256);
        auto reader=live2dstruct::CReader::FromBytes(bytes);live2dstruct::CSectionTable table(reader);QVERIFY(table.IsValid());
        int index=-1;for(int i=0;i<table.art_meshes.size();++i)if(table.art_meshes[i]->vertex_counts>4&&table.art_meshes[i]->position_index_sources_counts>6){index=i;break;}
        QVERIFY(index>=0);auto edit=identity(table,index);const int oldCount=edit.vertices.size();
        if(operation=="grow") {
            const int a=edit.indices[0],b=edit.indices[1],c=edit.indices[2];edit.vertices.append({{a,b},{.5,.5}});
            edit.indices[1]=quint16(oldCount);edit.indices.append(quint16(oldCount));edit.indices.append(quint16(b));edit.indices.append(quint16(c));
        } else if(operation=="delete") {
            edit.vertices.removeLast();edit.oldToNew.last()=-1;QVector<quint16> kept;
            for(int i=0;i<edit.indices.size();i+=3)if(edit.indices[i]!=oldCount-1&&edit.indices[i+1]!=oldCount-1&&edit.indices[i+2]!=oldCount-1)
                for(int j=0;j<3;++j)kept.append(edit.indices[i+j]);
            edit.indices.swap(kept);QVERIFY(!edit.indices.isEmpty());
        } else {
            std::swap(edit.vertices[0],edit.vertices[1]);std::swap(edit.oldToNew[0],edit.oldToNew[1]);for(auto& i:edit.indices)i=quint16(edit.oldToNew[i]);
        }
        Builder::Request request;request.meshes.append(edit);request.gluePolicy=Builder::GlueEndpointPolicy::DropPair;Builder::Result result;QString error;
        QVERIFY2(Builder::Build(bytes,request,result,&error),qPrintable(error));QVERIFY(result.bytes!=bytes);
        ModelOwner source;QVERIFY(source.Load(bytes));auto* oldModel=source.model;
        const auto name=edit.artMeshId;const int drawable=oldModel->GetDrawableIndex(CubismFramework::GetIdManager()->GetId(name.toUtf8().constData()));QVERIFY(drawable>=0);
        for(int p=0;p<oldModel->GetParameterCount();++p)oldModel->SetParameterValue(p,oldModel->GetParameterMinimumValue(p)*.3f+oldModel->GetParameterMaximumValue(p)*.7f);
        oldModel->SaveParameters();oldModel->SetParameterValue(0,oldModel->GetParameterDefaultValue(0));
        oldModel->SetParameterValue(CubismFramework::GetIdManager()->GetId("GeometryVirtual"),.75f);
        if(oldModel->GetPartCount())oldModel->SetPartOpacity(0,.43f);oldModel->SetModelOpacity(.67f);oldModel->Update();
        auto* oldMesh=oldModel->GetMeshMap()->GetDrawMesh(name);QVERIFY(oldMesh);oldMesh->SetShow(true);oldMesh->SetIsOver(true);
        oldMesh->GetMesh()->GetWolrd()[0]->SetIsSelect(true);oldMesh->GetMesh()->GetWolrd()[oldCount-1]->SetIsSelect(true);
        CMeshEditorSnapshot original;QVERIFY(oldModel->GetMeshMap()->CaptureEditorState(original));CubismModel::RuntimeValueSnapshot values;QVERIFY(oldModel->CaptureRuntimeValues(values));
        CubismModelReplacement strict;QVERIFY(!strict.Prepare(reinterpret_cast<const csmByte*>(result.bytes.constData()),result.bytes.size(),*source.moc,*oldModel,*source.renderer,prepareRenderer(),&error));
        auto mapping=migration(result.meshes.first());const auto policy=CubismModelUpdatePolicy::AllowDrawableGeometryEdits;
        CubismModelReplacement rejected;const auto reject=[](CubismRenderer*,CubismModel*,const CubismRenderer&,QString* error){if(error)*error="controlled renderer rejection";return false;};
        QVERIFY(!rejected.Prepare(reinterpret_cast<const csmByte*>(result.bytes.constData()),result.bytes.size(),*source.moc,*oldModel,*source.renderer,reject,policy,{},&error,{},&mapping));
        QCOMPARE(source.model,oldModel);QVERIFY(oldMesh->GetMesh()->GetWolrd()[oldCount-1]->GetIsSelect());
        CubismModelReplacement candidate;QVERIFY2(candidate.Prepare(reinterpret_cast<const csmByte*>(result.bytes.constData()),result.bytes.size(),*source.moc,*oldModel,*source.renderer,
            prepareRenderer(),policy,{},&error,{},&mapping),qPrintable(error));
        auto* next=candidate.GetModel();QCOMPARE(next->GetDrawableVertexCount(drawable),edit.vertices.size());
        CubismModel::RuntimeValueSnapshot nextValues;QVERIFY(next->CaptureRuntimeValues(nextValues));QCOMPARE(nextValues.parameterValues,values.parameterValues);
        QCOMPARE(nextValues.savedParameters,values.savedParameters);QCOMPARE(nextValues.savedPresence,values.savedPresence);QCOMPARE(nextValues.partOpacities,values.partOpacities);QCOMPARE(nextValues.modelOpacity,values.modelOpacity);
        QCOMPARE(next->GetParameterValue(CubismFramework::GetIdManager()->GetId("GeometryVirtual")),.75f);
        auto* nextMesh=next->GetMeshMap()->GetDrawMesh(name);QVERIFY(nextMesh);QVERIFY(nextMesh->IsShown());QVERIFY(nextMesh->IsHovered());
        for(int old:{0,oldCount-1})if(edit.oldToNew[old]>=0)QVERIFY(nextMesh->GetMesh()->GetWolrd()[edit.oldToNew[old]]->GetIsSelect());
        if(operation=="grow")QVERIFY(!nextMesh->GetMesh()->GetWolrd().back()->GetIsSelect());
        QVERIFY(candidate.Commit(source.moc,source.model,source.renderer));
        CMeshGeometryMigration undo;undo.remaps.insert(name,{int(edit.vertices.size()),oldCount,result.meshes.first().newToOld});undo.targetState=original;
        CubismModelReplacement restored;QVERIFY2(restored.Prepare(reinterpret_cast<const csmByte*>(bytes.constData()),bytes.size(),*source.moc,*source.model,*source.renderer,
            prepareRenderer(),policy,{},&error,{},&undo),qPrintable(error));QVERIFY(restored.Commit(source.moc,source.model,source.renderer));
        auto* restoredMesh=source.model->GetMeshMap()->GetDrawMesh(name);QVERIFY(restoredMesh->GetMesh()->GetWolrd()[oldCount-1]->GetIsSelect());
        QCOMPARE(source.model->GetDrawableVertexCount(drawable),oldCount);QCOMPARE(QCryptographicHash::hash(read(path),QCryptographicHash::Sha256),digest);
    }
};
QTEST_GUILESS_MAIN(MeshGeometryReplacementCoreTests)
#include "mesh_geometry_replacement_core_tests.moc"
