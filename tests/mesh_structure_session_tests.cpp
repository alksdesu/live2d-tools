#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/cmocmeshstructurebuilder.h"
#include "live2dstruct/csectiontable.h"
#include <QtTest>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>

using namespace live2dstruct;
using MeshBuilder = CMocMeshStructureBuilder;
using ParameterBuilder = CMocParameterStructureBuilder;
namespace {
CModelSession capture(const CModelDocument& document)
{
    CModelSession result; result.sourceMoc=document.SourcePath(); result.sourceDigest=document.SourceDigest();
    result.structure=document.ParameterStructure(); result.meshStructure=document.HasMeshStructure();
    result.version=result.meshStructure ? 5 : result.structure.columns.isEmpty() && result.structure.counts.isEmpty() ? 1 : 2;
    for(auto it=document.ParameterValues().cbegin();it!=document.ParameterValues().cend();++it) result.parameters.insert(it.key(),it.value());
    result.parameterMetadata=document.ParameterMetadataOverrides(); result.visibility=document.VisibilityState();
    for(const auto& key:document.LockedKeys()) result.locked.insert(key,true);
    result.drawableOpacity=document.DrawableOpacityOverrides(); result.drawableDrawOrder=document.DrawableDrawOrderOverrides();
    result.drawableKeyformPositions=document.DrawableKeyformPositionOverrides(); result.drawableIndices=document.DrawableIndexOverrides();
    result.drawableMaskIndices=document.DrawableMaskIndexOverrides(); result.drawableMaskInverted=document.DrawableMaskInvertedOverrides();
    result.deformerParentIndices=document.DeformerParentOverrides(); result.deformerRotationKeyforms=document.DeformerRotationKeyformOverrides();
    result.deformerWarpKeyforms=document.DeformerWarpKeyformOverrides(); return result;
}
MeshBuilder::MeshEdit identity(const CModelDocument& document,const QString& id)
{
    MeshBuilder::MeshEdit result; result.artMeshId=id;
    for(const auto& mesh:document.Drawables()) if(mesh.id==id) {
        for(int index=0;index<mesh.vertexCount;++index) {result.vertices.append({{index},{1}});result.oldToNew.append(index);}
        result.indices=document.DrawableIndices(id); break;
    }
    return result;
}
bool appendPoint(CModelDocument& document,const QString& id,QString* error)
{
    auto mesh=identity(document,id); mesh.vertices.append({{0,1},{.25,.75}});
    MeshBuilder::Request request; request.meshes.append(mesh); CModelEdit edit;
    return document.MakeMeshStructureEdit(request,edit,error) && document.ExecuteEdit(edit);
}
bool write(const QString& path,const QByteArray& bytes)
{
    QFile file(path);return file.open(QIODevice::WriteOnly)&&file.write(bytes)==bytes.size();
}
}
class MeshStructureSessionTests : public QObject {
    Q_OBJECT
    CModelDocument m_original;
    QString m_target;
    void compareRestored(const CModelSession& session,const CModelDocument& expected) {
        CModelSession decoded; QString error; const auto json=session.ToJson(); QVERIFY(!json.isEmpty());
        QVERIFY2(CModelSession::FromJson(json,&decoded,&error),qPrintable(error)); QCOMPARE(decoded.meshStructure,session.meshStructure);
        QCOMPARE(decoded.structure,session.structure);
        const CModelDocument* sources[]{&m_original,&expected};
        for(const auto* loaded:sources) {
            CModelDocument restored;
            QVERIFY2(decoded.RestoreDocument(*loaded,restored,&error),qPrintable(error));
            QCOMPARE(restored.AuthoringStructureState(),expected.AuthoringStructureState());
            QCOMPARE(restored.SourceBytes(),m_original.SourceBytes()); QCOMPARE(restored.SourceDigest(),m_original.SourceDigest());
            QVERIFY(!restored.CanUndoEdit()); QVERIFY(!restored.CanRedoEdit()); QByteArray wanted,actual;
            QVERIFY(expected.BuildPreviewMoc(wanted,&error)); QVERIFY(restored.BuildPreviewMoc(actual,&error)); QCOMPARE(actual,wanted);
        }
    }
    CModelSession meshSession() {
        CModelDocument document=m_original; QString error;
        if(!appendPoint(document,m_target,&error)) return {};
        return capture(document);
    }
private slots:
    void initTestCase() {
        QString error; QVERIFY2(m_original.Load(QString(TEST_RESOURCES)+"/Mark/Mark.moc3",&error),qPrintable(error));
        for(const auto& mesh:m_original.Drawables()) if(mesh.vertexCount>=3&&mesh.indices.size()>=6&&!mesh.keyforms.isEmpty()) {m_target=mesh.id;break;}
        QVERIFY(!m_target.isEmpty());
    }
    void analysisPreservesDocumentAndRejectsLockedOrInvalidTargets() {
        CModelDocument document=m_original; QString error;
        auto mesh=identity(document,m_target); mesh.vertices.append({{0,1},{.5,.5}});
        MeshBuilder::Request request; request.meshes.append(mesh);
        const auto state=document.AuthoringStructureState(); const auto revision=document.Revision();
        MeshBuilder::Impact impact;
        QVERIFY2(document.AnalyzeMeshStructureEdit(request,impact,&error),qPrintable(error));
        QCOMPARE(impact.meshes.size(),1); QCOMPARE(impact.meshes[0].newVertexCount,mesh.vertices.size());
        QCOMPARE(document.AuthoringStructureState(),state); QCOMPARE(document.Revision(),revision); QVERIFY(!document.CanUndoEdit());
        document.SetLocked("artmesh",m_target,true);
        impact.blockingReasons={"keep"};
        QVERIFY(!document.AnalyzeMeshStructureEdit(request,impact,&error)); QVERIFY(!error.isEmpty());
        QCOMPARE(impact.blockingReasons,QStringList{"keep"});
        document.SetLocked("artmesh",m_target,false);
        request.meshes[0].artMeshId="missing";
        QVERIFY(!document.AnalyzeMeshStructureEdit(request,impact,&error)); QCOMPARE(impact.blockingReasons,QStringList{"keep"});
        request.meshes.clear();
        QVERIFY(!document.AnalyzeMeshStructureEdit(request,impact,&error)); QCOMPARE(impact.blockingReasons,QStringList{"keep"});
    }
    void mixedMeshParameterHistoryAndSessionStayIndependent() {
        CModelDocument document=m_original; QString error; QVector<QVariantMap> states{document.AuthoringStructureState()}; QVector<QByteArray> previews;
        QByteArray preview; QVERIFY(document.BuildPreviewMoc(preview,&error)); previews.append(preview);
        const auto remember=[&]() {
            states.append(document.AuthoringStructureState()); QByteArray bytes;
            if(!document.BuildPreviewMoc(bytes,&error)) return false;
            previews.append(bytes); return true;
        };
        const auto originalPositions=document.DrawableKeyformPositions(m_target,0); auto changed=originalPositions; changed[0]+=.125f;
        CModelEdit position{m_target,"artmesh",CModelEdit::KeyformPositions,QVariant::fromValue(originalPositions),QVariant::fromValue(changed),0,false};
        QVERIFY(document.ExecuteEdit(position)); QVERIFY(remember());
        const auto originalIndices=document.DrawableIndices(m_target); auto altered=originalIndices; altered.resize(altered.size()-3);
        CModelEdit topology{m_target,"artmesh",CModelEdit::MeshIndices,QVariant::fromValue(originalIndices),QVariant::fromValue(altered),-1,false};
        QVERIFY(document.ExecuteEdit(topology)); QVERIFY(remember());
        QVERIFY2(appendPoint(document,m_target,&error),qPrintable(error)); QVERIFY(document.HasMeshStructure()); QVERIFY(remember());
        QCOMPARE(document.DrawableIndices(m_target),altered); QVERIFY(!document.DrawableIndexOverrides().contains(m_target));
        QVERIFY(!document.DrawableKeyformPositionOverrides().contains(m_target)); QCOMPARE(document.DrawableKeyformPositions(m_target,0)[0],changed[0]);
        ParameterBuilder::CreateRequest create; create.parameter.id="ParamMixedMesh"; CModelEdit createEdit;
        QVERIFY2(document.MakeCreateParameterEdit(create,"混合结构参数",createEdit,&error),qPrintable(error)); QVERIFY(document.ExecuteEdit(createEdit)); QVERIFY(remember());
        CModelEdit binding;
        QVERIFY2(document.MakeBindParameterEdit(create.parameter.id,{{{ParameterBuilder::Owner::ArtMesh,m_target},{-1,0,1}}},binding,&error),qPrintable(error));
        QVERIFY(document.ExecuteEdit(binding)); QVERIFY(remember());
        auto remove=identity(document,m_target); remove.vertices.removeLast(); remove.oldToNew.last()=-1;
        MeshBuilder::Request deletion; deletion.meshes.append(remove); CModelEdit removeEdit;
        QVERIFY2(document.MakeMeshStructureEdit(deletion,removeEdit,&error),qPrintable(error)); QVERIFY(document.ExecuteEdit(removeEdit)); QVERIFY(remember());
        const auto before=document.DrawableKeyformPositions(m_target,0); auto after=before; after[1]+=.0625f;
        position={m_target,"artmesh",CModelEdit::KeyformPositions,QVariant::fromValue(before),QVariant::fromValue(after),0,false};
        QVERIFY(document.ExecuteEdit(position)); QVERIFY(remember());
        const auto finalSession=capture(document); compareRestored(finalSession,document);
        QTemporaryDir directory; QVERIFY(directory.isValid()); QVERIFY2(finalSession.Save(directory.filePath("mixed.session"),&error),qPrintable(error));
        CModelSession saved; QVERIFY(CModelSession::Load(directory.filePath("mixed.session"),&saved,&error)); compareRestored(saved,document);
        for(int i=states.size()-1;i>0;--i) {
            QVERIFY2(document.UndoEdit(),qPrintable(QString("undo %1").arg(i))); QCOMPARE(document.AuthoringStructureState(),states[i-1]);
            QVERIFY(document.BuildPreviewMoc(preview,&error)); QCOMPARE(preview,previews[i-1]); compareRestored(capture(document),document);
        }
        QVERIFY(!document.CanUndoEdit()); QVERIFY(!document.HasMeshStructure());
        for(int i=1;i<states.size();++i) {
            QVERIFY2(document.RedoEdit(),qPrintable(QString("redo %1").arg(i))); QCOMPARE(document.AuthoringStructureState(),states[i]);
            QVERIFY(document.BuildPreviewMoc(preview,&error)); QCOMPARE(preview,previews[i]);
        }
        QVERIFY(!document.CanRedoEdit()); QCOMPARE(document.SourceBytes(),m_original.SourceBytes()); QCOMPARE(document.SourceDigest(),m_original.SourceDigest());
    }
    void deletingBoundParameterAfterMeshExpansionKeepsGeometry() {
        CModelDocument document=m_original;QString error;QVERIFY2(appendPoint(document,m_target,&error),qPrintable(error));
        ParameterBuilder::CreateRequest request;request.parameter.id="ParamDeleteAfterMesh";
        request.bindings={{{ParameterBuilder::Owner::ArtMesh,m_target},{-1,1}}};CModelEdit created;
        QVERIFY2(document.MakeCreateParameterEdit(request,{},created,&error),qPrintable(error));QVERIFY(document.ExecuteEdit(created));
        const auto before=document.AuthoringStructureState();const auto geometry=identity(document,m_target);
        const CModelDrawable* selected=nullptr;for(const auto& mesh:document.Drawables())if(mesh.id==m_target)selected=&mesh;QVERIFY(selected);
        const auto uv=selected->uvs;const int count=selected->vertexCount;
        CModelEdit deleted;QVERIFY2(document.MakeDeleteParameterEdit(ParameterBuilder::DeleteRequest{request.parameter.id,.25f},deleted,&error),qPrintable(error));QVERIFY(document.ExecuteEdit(deleted));
        QVERIFY(document.HasMeshStructure());QCOMPARE(document.DrawableIndices(m_target),geometry.indices);
        for(const auto& mesh:document.Drawables())if(mesh.id==m_target){QCOMPARE(mesh.vertexCount,count);QCOMPARE(mesh.uvs,uv);}
        compareRestored(capture(document),document);const auto after=document.AuthoringStructureState();
        QVERIFY(document.UndoEdit());QCOMPARE(document.AuthoringStructureState(),before);QVERIFY(document.RedoEdit());QCOMPARE(document.AuthoringStructureState(),after);
    }
    void versionFiveWithoutMeshKeepsLegacyParameterStructure() {
        CModelDocument document=m_original;ParameterBuilder::CreateRequest request;request.parameter.id="ParamLegacyV5";CModelEdit edit;QString error;
        QVERIFY(document.MakeCreateParameterEdit(request,{},edit,&error));QVERIFY(document.ExecuteEdit(edit));QVERIFY(!document.HasMeshStructure());
        auto session=capture(document);session.version=5;QVERIFY(!session.meshStructure);compareRestored(session,document);
        const auto json=QJsonDocument::fromJson(session.ToJson()).object();QCOMPARE(json["version"].toInt(),5);QVERIFY(json["meshStructure"].isBool());QVERIFY(!json["meshStructure"].toBool());
    }
    void storedVersionUpgradesWithoutLosingPhysicsDraft_data() {
        QTest::addColumn<int>("version");QTest::addColumn<bool>("physics");
        for(int version=1;version<=5;++version) for(bool physics:{false,true}) QTest::newRow(qPrintable(QString("%1-%2").arg(version).arg(physics)))<<version<<physics;
    }
    void storedVersionUpgradesWithoutLosingPhysicsDraft() {
        QFETCH(int,version);QFETCH(bool,physics);auto session=meshSession();QVERIFY(session.meshStructure);session.version=version;
        if(physics) session.physicsDraft=CModelSession::PhysicsDraft{{},QJsonObject{{"Version",3},{"PhysicsSettings",QJsonArray{}}},true};
        const auto bytes=session.ToJson();QVERIFY(!bytes.isEmpty());const auto json=QJsonDocument::fromJson(bytes).object();QCOMPARE(json["version"].toInt(),5);QVERIFY(json["meshStructure"].isBool());QVERIFY(json["meshStructure"].toBool());
        CModelSession parsed;QString error;QVERIFY2(CModelSession::FromJson(bytes,&parsed,&error),qPrintable(error));QCOMPARE(parsed.version,5);QCOMPARE(parsed.physicsDraft.has_value(),physics);QCOMPARE(session.version,version);
    }
    void legacyVersionRejectsAnyMeshMarker_data() {
        QTest::addColumn<int>("version");QTest::addColumn<QJsonValue>("flag");
        const QVector<QJsonValue> values{false,true,QJsonValue(),0,"true",QJsonObject{}};
        for(int version=1;version<=4;++version) for(int type=0;type<values.size();++type) QTest::newRow(qPrintable(QString("%1-%2").arg(version).arg(type)))<<version<<values[type];
    }
    void legacyVersionRejectsAnyMeshMarker() {
        QFETCH(int,version);QFETCH(QJsonValue,flag);CModelSession result;result.sourceMoc="keep";QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(QJsonObject{{"version",version},{"meshStructure",flag}}).toJson(),&result,&error));QCOMPARE(result.sourceMoc,QString("keep"));
        QVERIFY2(CModelSession::FromJson(QJsonDocument(QJsonObject{{"version",version}}).toJson(),&result,&error),qPrintable(error));QVERIFY(!result.meshStructure);QCOMPARE(result.version,version);
        const auto json=QJsonDocument::fromJson(result.ToJson()).object();QVERIFY(!json.contains("meshStructure"));
    }
    void versionFiveRequiresTypedMarker_data() {
        QTest::addColumn<QJsonValue>("flag");
        QTest::newRow("missing")<<QJsonValue(QJsonValue::Undefined);QTest::newRow("null")<<QJsonValue();QTest::newRow("number")<<QJsonValue(1);QTest::newRow("string")<<QJsonValue("true");QTest::newRow("object")<<QJsonValue(QJsonObject{});
    }
    void versionFiveRequiresTypedMarker() {
        QFETCH(QJsonValue,flag);QJsonObject json{{"version",5}};if(!flag.isUndefined())json["meshStructure"]=flag;
        CModelSession result;result.sourceMoc="keep";QString error;QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(),&result,&error));QCOMPARE(result.sourceMoc,QString("keep"));
        json["meshStructure"]=false;QVERIFY(CModelSession::FromJson(QJsonDocument(json).toJson(),&result,&error));QVERIFY(!result.meshStructure);
        QCOMPARE(QJsonDocument::fromJson(result.ToJson()).object()["meshStructure"].toBool(true),false);
    }
    void malformedAnchorsFailBeforeSaveOrRestore_data() {
        QTest::addColumn<int>("mutation");for(int i=0;i<6;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void malformedAnchorsFailBeforeSaveOrRestore() {
        QFETCH(int,mutation);auto session=meshSession();QVERIFY(session.meshStructure);const auto good=QJsonDocument::fromJson(session.ToJson()).object();auto json=good;
        if(mutation==0) {session.sourceMoc.clear();json.remove("sourceMoc");}
        if(mutation==1) {session.sourceMoc=QString("bad")+QChar::Null+"path";json["sourceMoc"]=session.sourceMoc;}
        if(mutation==2) {session.sourceDigest.clear();json.remove("sourceDigest");}
        if(mutation==3) {session.sourceDigest=QByteArray(31,'x');json["sourceDigest"]=QString::fromLatin1(session.sourceDigest.toHex());}
        if(mutation==4) {session.sourceDigest=QByteArray(33,'x');json["sourceDigest"]=QString::fromLatin1(session.sourceDigest.toHex());}
        if(mutation==5) {session.version=13;json["version"]=13;}
        QVERIFY(session.ToJson().isEmpty());CModelSession decoded;decoded.sourceMoc="keep";QString error;
        QVERIFY(!CModelSession::FromJson(QJsonDocument(json).toJson(),&decoded,&error));QCOMPARE(decoded.sourceMoc,QString("keep"));
        CModelDocument target=m_original;const auto before=target.AuthoringStructureState();const auto revision=target.Revision();
        QVERIFY(!session.RestoreDocument(m_original,target,&error));QCOMPARE(target.AuthoringStructureState(),before);QCOMPARE(target.Revision(),revision);
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("keep.session");QVERIFY(write(path,"old"));QVERIFY(!session.Save(path,&error));QFile preserved(path);QVERIFY(preserved.open(QIODevice::ReadOnly));QCOMPARE(preserved.readAll(),QByteArray("old"));
    }
    void wrongModeDigestAndStaleOverlayPreserveHistory_data() {
        QTest::addColumn<int>("mutation");for(int i=0;i<5;++i) QTest::newRow(qPrintable(QString::number(i)))<<i;
    }
    void wrongModeDigestAndStaleOverlayPreserveHistory() {
        QFETCH(int,mutation);auto session=meshSession();QVERIFY(session.meshStructure);
        if(mutation==0) {
            CModelDocument strict=m_original;QString detail;const auto before=strict.AuthoringStructureState();
            QVERIFY(!strict.SetParameterStructure(session.structure,&detail));QCOMPARE(strict.AuthoringStructureState(),before);
            session.meshStructure=false;
        }
        if(mutation==1) session.sourceDigest=QByteArray(32,'x');
        if(mutation==2) session.drawableKeyformPositions[m_target]={m_original.DrawableKeyformPositions(m_target,0)};
        if(mutation==3) session.version=4;
        if(mutation==4) session.drawableIndices[m_target]={0,1,32767};
        CModelDocument target=m_original;QString error;QVERIFY(appendPoint(target,m_target,&error));QVERIFY(appendPoint(target,m_target,&error));QVERIFY(target.UndoEdit());
        const auto before=target.AuthoringStructureState();const auto revision=target.Revision();QVERIFY(target.CanUndoEdit());QVERIFY(target.CanRedoEdit());
        QVERIFY(!session.RestoreDocument(m_original,target,&error));QVERIFY(!error.isEmpty());QCOMPARE(target.AuthoringStructureState(),before);QCOMPARE(target.Revision(),revision);QVERIFY(target.CanUndoEdit());QVERIFY(target.CanRedoEdit());QVERIFY(target.RedoEdit());
    }
    void restorationUsesImmutableLoadedSnapshotAfterDiskChanges() {
        QTemporaryDir directory;QVERIFY(directory.isValid());const auto path=directory.filePath("source.moc3");QVERIFY(write(path,m_original.SourceBytes()));
        CModelDocument loaded;QString error;QVERIFY(loaded.Load(path,&error));QVERIFY(appendPoint(loaded,m_target,&error));const auto session=capture(loaded);
        QVERIFY(write(path,"different disk bytes"));CModelDocument restored;
        QVERIFY2(session.RestoreDocument(loaded,restored,&error),qPrintable(error));QCOMPARE(restored.AuthoringStructureState(),loaded.AuthoringStructureState());
        QVERIFY(QFile::remove(path));QVERIFY(session.RestoreDocument(loaded,restored,&error));QCOMPARE(restored.SourceBytes(),m_original.SourceBytes());QCOMPARE(restored.SourceDigest(),m_original.SourceDigest());
    }
};
QTEST_GUILESS_MAIN(MeshStructureSessionTests)
#include "mesh_structure_session_tests.moc"
