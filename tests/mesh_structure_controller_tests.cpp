#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectionvalidation.h"
#include <limits>
#include "deformer_snapshot_fixture.h"
#include <QtTest>
#include <stdexcept>

using namespace live2dstruct;
namespace {
using Builder=CMocMeshStructureBuilder;
Builder::Request appendRequest(const CModelDocument& document,const QString& id)
{
    Builder::MeshEdit mesh;mesh.artMeshId=id;
    for(const auto& drawable:document.Drawables())if(drawable.id==id) {
        for(int i=0;i<drawable.vertexCount;++i){mesh.vertices.append({{i},{1}});mesh.oldToNew.append(i);}
        mesh.indices=document.DrawableIndices(id);break;
    }
    mesh.vertices.append({{0,1},{.5,.5}});Builder::Request request;request.meshes.append(mesh);return request;
}
struct Preview {
    int attempts=0,accepted=0,structures=0,ended=0;
    bool reject=false,throws=false,dirtyFailure=false,restore=false;
    QByteArray bytes;
    QVector<int> selected{0};
    std::function<void()> during;
    void connect(CKeyformEditController& controller) {
        controller.SetPublisher([this](const CModelDocument& document,const auto&,bool,QString* error){return document.BuildPreviewMoc(bytes,error);},
            [this](QString*){++ended;return true;});
        controller.SetMeshPublisher([this](const CModelDocument& document,const CModelEdit& command,bool forward,QString* error) {
            ++attempts;if(during)during();
            if(dirtyFailure){command.runtime->before="bad-before";command.runtime->after="bad-after";command.runtime->beforeEditor="bad-editor-before";command.runtime->afterEditor="bad-editor-after";}
            if(throws)throw std::runtime_error("rejected mesh publication");
            if(reject){if(error)*error="mesh publication rejected";return false;}
            QByteArray candidate;if(!document.BuildPreviewMoc(candidate,error))return false;
            const auto& stored=forward?command.runtime->after:command.runtime->before;
            QVector<int> next;
            if(stored.isValid())next=stored.value<QVector<int>>();
            else {
                const auto& mapping=forward?command.meshRemaps.first().oldToNew:command.meshRemaps.first().newToOld;
                for(int index:selected)if(index>=0&&index<mapping.size()&&mapping[index]>=0)next.append(mapping[index]);
            }
            QVariant leaving=QVariant::fromValue(selected);(forward?command.runtime->before:command.runtime->after).swap(leaving);
            bytes.swap(candidate);selected.swap(next);++accepted;return true;
        });
        controller.SetStructurePublisher([this](const CModelDocument& document,const CModelEdit& command,bool,bool restoring,
            const auto&,bool,auto& pose,QString* error){
            ++structures;
            if(dirtyFailure){command.runtime->before="bad-before";command.runtime->after="bad-after";command.runtime->beforeEditor="bad-editor-before";command.runtime->afterEditor="bad-editor-after";}
            if(throws)throw std::runtime_error("rejected structure publication");
            if(reject){if(error)*error="structure rejected";return false;}
            QByteArray candidate;if(!document.BuildPreviewMoc(candidate,error))return false;
            for(const auto& parameter:document.Parameters())pose.insert(parameter.id,document.ParameterValue(parameter.id));
            bytes.swap(candidate);restore=restoring;return true;
        });
    }
};
}
class MeshStructureControllerTests : public QObject {
    Q_OBJECT
    CModelDocument m_original;
    QString m_mesh;
    bool make(CModelDocument& document,CModelEdit& edit,QString& error) {return document.MakeMeshStructureEdit(appendRequest(document,m_mesh),edit,&error);}
private slots:
    void initTestCase() {
        QString error;QVERIFY2(m_original.Load(DeformerSnapshotData::sample("Mark"),&error),qPrintable(error));
        for(const auto& mesh:m_original.Drawables())if(mesh.vertexCount>=3&&mesh.indices.size()>=3){m_mesh=mesh.id;break;}
        QVERIFY(!m_mesh.isEmpty());
    }
    void publisherFailurePreservesDocumentAndRedo_data() {
        QTest::addColumn<int>("fault");for(int i=0;i<3;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void publisherFailurePreservesDocumentAndRedo() {
        QFETCH(int,fault);CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;
        CModelEdit edit;QVERIFY(make(document,edit,error));QVERIFY(controller.Submit(edit,&error));QVERIFY(make(document,edit,error));QVERIFY(controller.Submit(edit,&error));
        CModelEdit history;QVERIFY(controller.History(true,history,&error));const auto state=document.AuthoringStructureState();const auto revision=document.Revision();const auto bytes=preview.bytes;const auto selection=QVariant::fromValue(preview.selected);const int accepted=preview.accepted;
        QVERIFY(make(document,edit,error));if(fault==0)controller.SetMeshPublisher({});preview.reject=fault==1;preview.throws=fault==2;
        QVERIFY(!controller.Submit(edit,&error));QVERIFY(!error.isEmpty());QCOMPARE(document.AuthoringStructureState(),state);QCOMPARE(document.Revision(),revision);QCOMPARE(preview.bytes,bytes);QCOMPARE(QVariant::fromValue(preview.selected),selection);QCOMPARE(preview.accepted,accepted);QVERIFY(document.CanUndoEdit());QVERIFY(document.CanRedoEdit());
    }
    void historyRestoresDeletedVertexSelectionAndSidecars() {
        CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit edit;
        QVERIFY(make(document,edit,error));const int originalCount=edit.meshRemaps.first().oldVertexCount;QVERIFY(controller.Submit(edit,&error));preview.selected={originalCount};
        auto request=appendRequest(document,m_mesh);auto& mesh=request.meshes[0];mesh.vertices.removeLast();mesh.vertices.removeLast();mesh.oldToNew.last()=-1;
        QVERIFY(document.MakeMeshStructureEdit(request,edit,&error));QVERIFY(controller.Submit(edit,&error));QVERIFY(preview.selected.isEmpty());
        const auto after=document.AuthoringStructureState();CModelEdit history;QVERIFY(controller.History(true,history,&error));QCOMPARE(preview.selected,QVector<int>{originalCount});
        QVERIFY(history.runtime);QCOMPARE(history.runtime->before.value<QVector<int>>(),QVector<int>{originalCount});QVERIFY(history.runtime->after.isValid());QVERIFY(history.runtime->after.value<QVector<int>>().isEmpty());
        QVERIFY(controller.History(false,history,&error));QVERIFY(preview.selected.isEmpty());QCOMPARE(document.AuthoringStructureState(),after);
    }
    void rejectedHistoryDoesNotMutateStoredSidecars_data() {
        QTest::addColumn<bool>("throws");QTest::newRow("false")<<false;QTest::newRow("exception")<<true;
    }
    void rejectedHistoryDoesNotMutateStoredSidecars() {
        QFETCH(bool,throws);CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit edit;
        QVERIFY(make(document,edit,error));QVERIFY(controller.Submit(edit,&error));CModelEdit stored;QVERIFY(document.PeekUndoEdit(&stored));QVERIFY(stored.runtime);
        const CModelEditRuntimeState sidecar=*stored.runtime;const auto state=document.AuthoringStructureState();const auto revision=document.Revision();
        preview.dirtyFailure=true;preview.throws=throws;preview.reject=!throws;CModelEdit output;output.name="keep";
        QVERIFY(!controller.History(true,output,&error));QCOMPARE(output.name,QString("keep"));QCOMPARE(document.AuthoringStructureState(),state);QCOMPARE(document.Revision(),revision);
        QVERIFY(document.PeekUndoEdit(&stored));QCOMPARE(stored.runtime->before,sidecar.before);QCOMPARE(stored.runtime->after,sidecar.after);QCOMPARE(stored.runtime->beforeEditor,sidecar.beforeEditor);QCOMPARE(stored.runtime->afterEditor,sidecar.afterEditor);
        preview.dirtyFailure=false;preview.throws=false;preview.reject=false;QVERIFY(controller.History(true,output,&error));QVERIFY(controller.History(false,output,&error));
    }
    void parameterHistorySidecarsAlsoRollBack_data() {
        QTest::addColumn<bool>("throws");QTest::newRow("false")<<false;QTest::newRow("exception")<<true;
    }
    void parameterHistorySidecarsAlsoRollBack() {
        QFETCH(bool,throws);CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);
        CMocParameterStructureBuilder::CreateRequest request;request.parameter.id="ParameterSidecar";CModelEdit edit;QString error;
        QVERIFY(document.MakeCreateParameterEdit(request,{},edit,&error));QVERIFY(controller.Submit(edit,&error));QVERIFY(document.PeekUndoEdit(&edit));QVERIFY(edit.runtime);
        const auto sidecar=*edit.runtime;const auto before=document.AuthoringStructureState();const auto revision=document.Revision();
        preview.dirtyFailure=true;preview.throws=throws;preview.reject=!throws;CModelEdit output;output.name="keep";
        QVERIFY(!controller.History(true,output,&error));QCOMPARE(output.name,QString("keep"));QCOMPARE(document.AuthoringStructureState(),before);QCOMPARE(document.Revision(),revision);QVERIFY(document.PeekUndoEdit(&edit));
        QCOMPARE(edit.runtime->before,sidecar.before);QCOMPARE(edit.runtime->after,sidecar.after);QCOMPARE(edit.runtime->beforeEditor,sidecar.beforeEditor);QCOMPARE(edit.runtime->afterEditor,sidecar.afterEditor);
        preview.dirtyFailure=false;preview.throws=false;preview.reject=false;QVERIFY(controller.History(true,output,&error));QVERIFY(controller.History(false,output,&error));
    }
    void authoringSnapshotRejectsNonfiniteUv_data() {
        QTest::addColumn<float>("value");QTest::newRow("nan")<<std::numeric_limits<float>::quiet_NaN();QTest::newRow("infinity")<<std::numeric_limits<float>::infinity();
    }
    void authoringSnapshotRejectsNonfiniteUv() {
        QFETCH(float,value);const auto& source=m_original.SourceBytes();auto reader=CReader::FromBytes(source);CSectionTable table(reader);QVERIFY(table.IsValid());
        CMocParameterStructureBuilder::ColumnState state;bool found=false;
        for(const auto& span:CSectionValidation::ArraySpans(*table.sectionOffsetTable,*table.count_info))if(QByteArray(span.name)=="uvs.uvs") {
            auto data=source.mid(span.offset,span.count*span.width);quint32 bits;std::memcpy(&bits,&value,4);
            qToLittleEndian(bits,data.data()+table.art_meshes.first()->uv_sources_begin_indices*4);state.columns.insert(span.slot,data);found=true;
        }
        QVERIFY(found);CModelDocument target=m_original;const auto before=target.AuthoringStructureState();QString error;
        QVERIFY(!target.SetAuthoringStructure(state,&error));QVERIFY(!error.isEmpty());QCOMPARE(target.AuthoringStructureState(),before);
    }
    void sourceAndValueChangesRejectStaleMeshCandidates_data() {
        QTest::addColumn<bool>("source");QTest::newRow("value")<<false;QTest::newRow("source")<<true;
    }
    void sourceAndValueChangesRejectStaleMeshCandidates() {
        QFETCH(bool,source);CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit edit;QVERIFY(make(document,edit,error));
        if(source)QVERIFY(document.Load(DeformerSnapshotData::sample("Wanko"),&error));
        else {
            bool changed=false;for(const auto& parameter:document.Parameters())if(parameter.minimum<parameter.maximum) {
                const float value=document.ParameterValue(parameter.id)==parameter.minimum?parameter.maximum:parameter.minimum;
                QVERIFY(document.SetParameterValue(parameter.id,value));changed=true;break;
            }
            QVERIFY(changed);
        }
        const auto before=document.AuthoringStructureState();const auto revision=document.Revision();QVERIFY(!controller.Submit(edit,&error));QCOMPARE(preview.attempts,0);QCOMPARE(document.AuthoringStructureState(),before);QCOMPARE(document.Revision(),revision);
    }
    void publishingBlocksControllerReentryAndPublisherReplacement() {
        CModelDocument document=m_original,other=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit edit;QVERIFY(make(document,edit,error));
        bool nestedSubmit=true,nestedUndo=true,nestedRestore=true,nestedEnd=true;int substituteCalls=0;
        preview.during=[&]{CModelEdit history;QString nested;nestedSubmit=controller.Submit(edit,&nested);nestedUndo=controller.History(true,history,&nested);nestedRestore=controller.Restore(other,&nested,true);nestedEnd=controller.End(&nested);controller.SetDocument(&other);controller.SetMeshPublisher([&](const auto&,const auto&,bool,QString*){++substituteCalls;return false;});};
        QVERIFY2(controller.Submit(edit,&error),qPrintable(error));QVERIFY(!nestedSubmit);QVERIFY(!nestedUndo);QVERIFY(!nestedRestore);QVERIFY(!nestedEnd);QVERIFY(document.HasMeshStructure());QVERIFY(!other.HasMeshStructure());QCOMPARE(substituteCalls,0);
        preview.during={};QVERIFY(make(document,edit,error));QVERIFY(controller.Submit(edit,&error));QCOMPARE(substituteCalls,0);QCOMPARE(preview.accepted,2);
    }
    void wrongVertexMappingsFailBeforePublisher_data() {
        QTest::addColumn<int>("mutation");for(int i=0;i<6;++i)QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void wrongVertexMappingsFailBeforePublisher() {
        QFETCH(int,mutation);CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit edit;QVERIFY(make(document,edit,error));
        auto& map=edit.meshRemaps[0];
        if(mutation==0)map.oldToNew[0]=map.newVertexCount;
        if(mutation==1)map.newToOld[0]=-1;
        if(mutation==2)++map.oldVertexCount;
        if(mutation==3)map.artMeshId="missing";
        if(mutation==4)edit.meshRemaps.append(map);
        if(mutation==5)edit.meshRemaps.clear();
        const auto state=document.AuthoringStructureState();QVERIFY(!controller.Submit(edit,&error));QCOMPARE(document.AuthoringStructureState(),state);QCOMPARE(preview.attempts,0);QVERIFY(!document.CanUndoEdit());
    }
    void locksAreCheckedAtPreparationAndSubmission_data() {
        QTest::addColumn<int>("kind");QTest::newRow("mesh")<<0;QTest::newRow("deformer-ancestor")<<1;QTest::newRow("part-ancestor")<<2;
    }
    void locksAreCheckedAtPreparationAndSubmission() {
        QFETCH(int,kind);CModelDocument document=m_original;QString id,type,lockId;
        for(const auto& mesh:document.Drawables()) {
            if(mesh.vertexCount<3||mesh.indices.isEmpty()||(kind==1&&mesh.parentDeformerIndex<0)||(kind==2&&mesh.parentPartIndex<0))continue;
            id=mesh.id;type="artmesh";lockId=id;
            if(kind==1){const auto& parent=document.Deformers()[mesh.parentDeformerIndex];type=parent.type==CModelDeformer::Warp?"warp":"rotation";lockId=parent.id;}
            if(kind==2){type="part";lockId=document.Parts()[mesh.parentPartIndex].id;}break;
        }
        QVERIFY(!id.isEmpty());QString error;CModelEdit edit;const auto request=appendRequest(document,id);QVERIFY(document.MakeMeshStructureEdit(request,edit,&error));
        document.SetLocked(type,lockId,true);CModelEdit output;output.name="keep";QVERIFY(!document.MakeMeshStructureEdit(request,output,&error));QCOMPARE(output.name,QString("keep"));
        CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);const auto state=document.AuthoringStructureState();
        QVERIFY(!controller.Submit(edit,&error));QCOMPARE(preview.attempts,0);QCOMPARE(document.AuthoringStructureState(),state);
    }
    void activeKeyformSurvivesMeshHistoryAndRejectedChanges() {
        DeformerSnapshotData::Fixture fixture;CModelDocument document;QString error;QVERIFY(document.LoadBytes(fixture.bytes,"mapping-fixture.moc3",&error));CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);
        QVERIFY(controller.Activate("Mesh",1,{{"P",1}},&error));const auto pose=controller.Pose();const auto state=document.AuthoringStructureState();
        CModelEdit edit;QVERIFY(document.MakeMeshStructureEdit(appendRequest(document,"Mesh"),edit,&error));
        preview.reject=true;QVERIFY(!controller.Submit(edit,&error));QVERIFY(controller.IsActive());QCOMPARE(controller.Pose(),pose);QCOMPARE(document.AuthoringStructureState(),state);
        preview.reject=false;preview.throws=true;QVERIFY(!controller.Submit(edit,&error));QVERIFY(controller.IsActive());QCOMPARE(controller.Pose(),pose);QCOMPARE(document.AuthoringStructureState(),state);
        preview.throws=false;QVERIFY2(controller.Submit(edit,&error),qPrintable(error));QVERIFY(controller.IsActive());QCOMPARE(controller.Target(),QString("Mesh"));QCOMPARE(controller.Keyform(),1);QCOMPARE(controller.Pose(),pose);QCOMPARE(preview.ended,0);
        CModelEdit history;QVERIFY(controller.History(true,history,&error));QCOMPARE(document.AuthoringStructureState(),state);QVERIFY(controller.IsActive());QCOMPARE(controller.Pose(),pose);
        QVERIFY(controller.History(false,history,&error));QVERIFY(controller.IsActive());QCOMPARE(controller.Pose(),pose);
        CKeyformMappingContext mapping;QVERIFY(mapping.Build(document,"Mesh",1,controller.Pose(),&error));QVector<float> model;QVERIFY(mapping.ToModel(document.DrawableKeyformPositions("Mesh",1),model,&error));
        QVERIFY(document.MakeMeshStructureEdit(appendRequest(document,"Mesh"),edit,&error));const auto beforeDrag=document.AuthoringStructureState();const int attempts=preview.attempts;
        QVERIFY(controller.BeginDrag({0},model,&error));QVERIFY(!controller.Submit(edit,&error));QVERIFY(!controller.History(true,history,&error));QVERIFY(controller.IsDragging());QCOMPARE(document.AuthoringStructureState(),beforeDrag);QCOMPARE(preview.attempts,attempts);
        controller.FinishDrag();QVERIFY(controller.End(&error));QCOMPARE(preview.ended,1);QVERIFY(!controller.IsActive());QCOMPARE(document.AuthoringStructureState(),beforeDrag);
    }
    void mixedParameterAndMeshHistoryUseCorrectPublishers() {
        CModelDocument document=m_original;CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QString error;CModelEdit mesh;QVERIFY(make(document,mesh,error));QVERIFY(controller.Submit(mesh,&error));
        const auto first=document.AuthoringStructureState();CMocParameterStructureBuilder::CreateRequest create;create.parameter.id="MixedController";CModelEdit parameter;
        QVERIFY(document.MakeCreateParameterEdit(create,"mixed",parameter,&error));QVERIFY(controller.Submit(parameter,&error));QVERIFY(make(document,mesh,error));QVERIFY(controller.Submit(mesh,&error));const auto final=document.AuthoringStructureState();
        CModelEdit history;QVERIFY(controller.History(true,history,&error));QCOMPARE(history.field,CModelEdit::MeshStructure);QVERIFY(controller.History(true,history,&error));QCOMPARE(history.field,CModelEdit::ParameterStructure);QCOMPARE(document.AuthoringStructureState(),first);
        QVERIFY(controller.History(false,history,&error));QVERIFY(controller.History(false,history,&error));QCOMPARE(document.AuthoringStructureState(),final);QCOMPARE(preview.structures,3);QCOMPARE(preview.accepted,4);
        QVERIFY(make(document,mesh,error));mesh.field=CModelEdit::ParameterStructure;mesh.type="parameter";mesh.meshRemaps.clear();const auto before=document.AuthoringStructureState();QVERIFY(!controller.Submit(mesh,&error));QCOMPARE(document.AuthoringStructureState(),before);QCOMPARE(preview.structures,3);
    }
    void meshCommandCannotChangeParameterCatalog() {
        CModelDocument document=m_original;QString error;CModelEdit mesh;QVERIFY(make(document,mesh,error));CModelDocument forged=document;QVERIFY(forged.ExecuteEdit(mesh));
        CMocParameterStructureBuilder::CreateRequest request;request.parameter.id="SmuggledParameter";CModelEdit parameter;QVERIFY(forged.MakeCreateParameterEdit(request,{},parameter,&error));QVERIFY(forged.ExecuteEdit(parameter));mesh.after=forged.AuthoringStructureState();
        CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);QVERIFY(!controller.Submit(mesh,&error));QCOMPARE(preview.attempts,0);QCOMPARE(document.AuthoringStructureState(),m_original.AuthoringStructureState());
    }
    void meshCommandCannotRewritePartOrGlueKeyforms_data() {
        QTest::addColumn<QByteArray>("column");QTest::newRow("part-order")<<QByteArray("part_keyforms.draw_orders");QTest::newRow("glue-intensity")<<QByteArray("glue_keyforms.intensities");
    }
    void meshCommandCannotRewritePartOrGlueKeyforms() {
        QFETCH(QByteArray,column);CModelDocument document;QString error;QVERIFY(document.Load(DeformerSnapshotData::sample("Rice"),&error));
        QString target;for(const auto& mesh:document.Drawables())if(mesh.vertexCount>=3&&!mesh.indices.isEmpty()){target=mesh.id;break;}QVERIFY(!target.isEmpty());
        CModelEdit edit;QVERIFY2(document.MakeMeshStructureEdit(appendRequest(document,target),edit,&error),qPrintable(error));CModelDocument altered=document;QVERIFY(altered.ExecuteEdit(edit));
        const auto& bytes=altered.WorkingBaseBytes();auto reader=CReader::FromBytes(bytes);CSectionTable table(reader);QVERIFY(table.IsValid());auto state=altered.ParameterStructure();bool found=false;
        for(const auto& span:CSectionValidation::ArraySpans(*table.sectionOffsetTable,*table.count_info))if(QByteArray(span.name)==column) {
            QVERIFY(span.count>0);auto values=bytes.mid(span.offset,span.count*span.width);const quint32 bits=qFromLittleEndian<quint32>(values.constData());float original;std::memcpy(&original,&bits,4);
            DeformerSnapshotData::real(values,0,column.startsWith("glue_")?(original==0?.5f:original*.5f):original+.25f);state.columns.insert(span.slot,values);found=true;
        }
        QVERIFY(found);QVERIFY2(altered.SetAuthoringStructure(state,&error),qPrintable(error));edit.after=altered.AuthoringStructureState();
        CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);const auto before=document.AuthoringStructureState();
        QVERIFY(!controller.Submit(edit,&error));QCOMPARE(preview.attempts,0);QCOMPARE(document.AuthoringStructureState(),before);
    }
    void meshCommandCannotRewriteUnrelatedBlendShapeScalar() {
        DeformerSnapshotData::Fixture fixture;auto reader=CReader::FromBytes(fixture.bytes);CSectionTable table(reader);QVERIFY(table.IsValid());
        CMocParameterStructureBuilder::ColumnState state;
        const QMap<QByteArray,int> added{{"blend_shape_keyform_bindings.parameter_binding_sources_indices",-1},
            {"blend_shape_keyform_bindings.keyform_sources_blend_shape_indices",1},{"blend_shape_keyform_bindings.keyform_sources_blend_shape_counts",1},
            {"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_begin_indices",0},{"blend_shape_keyform_bindings.blend_shape_constraint_index_sources_counts",0},
            {"blend_shapes_warp_deformers.target_indices",0},{"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_begin_indices",0},
            {"blend_shapes_warp_deformers.blend_shape_keyform_binding_sources_counts",1}};
        int found=0,scalarSlot=-1;
        for(const auto& span:CSectionValidation::ArraySpans(*table.sectionOffsetTable,*table.count_info)) {
            const QByteArray name(span.name);
            if(name.startsWith("warp_deformer_keyforms.")) {
                if(!span.offset&&span.optional)continue;QCOMPARE(span.count,1);auto column=fixture.bytes.mid(span.offset,span.width);column.append(column);
                state.columns.insert(span.slot,column);state.counts.insert(span.countIndex,2);if(name=="warp_deformer_keyforms.opacities")scalarSlot=span.slot;
            } else if(added.contains(name)) {
                QByteArray column(4,'\0');qToLittleEndian(qint32(added[name]),column.data());state.columns.insert(span.slot,column);state.counts.insert(span.countIndex,1);++found;
            }
        }
        QCOMPARE(found,added.size());QVERIFY(scalarSlot>=0);QByteArray bytes;QString error;QVERIFY2(CMocParameterStructureBuilder::BuildColumnState(fixture.bytes,state,bytes,&error),qPrintable(error));
        CModelDocument document;QVERIFY2(document.LoadBytes(bytes,"blend-warp-fixture.moc3",&error),qPrintable(error));CModelEdit edit;
        QVERIFY2(document.MakeMeshStructureEdit(appendRequest(document,"Mesh"),edit,&error),qPrintable(error));CModelDocument altered=document;QVERIFY(altered.ExecuteEdit(edit));
        auto candidate=altered.ParameterStructure();auto afterReader=CReader::FromBytes(altered.WorkingBaseBytes());CSectionTable after(afterReader);QVERIFY(after.IsValid());
        QByteArray opacities=altered.WorkingBaseBytes().mid(after.sectionOffsetTable->warp_deformer_keyforms->opacities,after.warp_deformer_keyforms.size()*4);
        DeformerSnapshotData::real(opacities,4,.375f);candidate.columns.insert(scalarSlot,opacities);QVERIFY2(altered.SetAuthoringStructure(candidate,&error),qPrintable(error));edit.after=altered.AuthoringStructureState();
        CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);const auto before=document.AuthoringStructureState();
        QVERIFY(!controller.Submit(edit,&error));QCOMPARE(preview.attempts,0);QCOMPARE(document.AuthoringStructureState(),before);
    }
    void combinedSessionRestoreUsesStructurePublisherAtomically() {
        CModelDocument document=m_original,candidate=m_original;QString error;CModelEdit edit;QVERIFY(make(candidate,edit,error));QVERIFY(candidate.ExecuteEdit(edit));candidate.ClearEditHistory();const auto expected=candidate.AuthoringStructureState();
        CKeyformEditController controller;controller.SetDocument(&document);Preview preview;preview.connect(controller);preview.reject=true;QVERIFY(!controller.Restore(candidate,&error));QCOMPARE(document.AuthoringStructureState(),m_original.AuthoringStructureState());QCOMPARE(preview.attempts,0);
        preview.reject=false;QVERIFY(controller.Restore(candidate,&error));QCOMPARE(document.AuthoringStructureState(),expected);QVERIFY(preview.restore);QCOMPARE(preview.structures,2);QVERIFY(!document.CanUndoEdit());
    }
};
QTEST_GUILESS_MAIN(MeshStructureControllerTests)
#include "mesh_structure_controller_tests.moc"
