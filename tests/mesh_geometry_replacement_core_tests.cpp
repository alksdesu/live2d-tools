#include "framework/CubismFramework.hpp"
#include "framework/ICubismAllocator.hpp"
#include "framework/Model/CubismMoc.hpp"
#include "framework/Model/CubismModel.hpp"
#include "framework/Model/CubismModelReplacement.hpp"
#include "framework/Rendering/CubismRenderer.hpp"
#include "framework/Id/CubismIdManager.hpp"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
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
