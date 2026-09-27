#include "live2dstruct/calphameshsource.h"
#include "live2dstruct/cmodelsession.h"
#include "live2dstruct/csectiontable.h"
#include "real_core_model_fixture.h"
#include <QBuffer>
#include <QCryptographicHash>
using namespace live2dstruct;
using Planner = CAlphaMeshPlanner;
using Builder = CMocMeshStructureBuilder;
namespace {
int drawableIndex(csmModel* model, const QString& id)
{
    for (int i = 0; i < csmGetDrawableCount(model); ++i) if (QString::fromUtf8(csmGetDrawableIds(model)[i]) == id) return i;
    return -1;
}
QString unchangedVertices(csmModel* oldModel, csmModel* newModel, int target, const QVector<int>& oldToNew)
{
    if (csmGetDrawableCount(oldModel) != csmGetDrawableCount(newModel)) return "drawable count";
    for (int mesh = 0; mesh < csmGetDrawableCount(oldModel); ++mesh) {
        if (std::strcmp(csmGetDrawableIds(oldModel)[mesh], csmGetDrawableIds(newModel)[mesh]) ||
            csmGetDrawableTextureIndices(oldModel)[mesh] != csmGetDrawableTextureIndices(newModel)[mesh] ||
            csmGetDrawableConstantFlags(oldModel)[mesh] != csmGetDrawableConstantFlags(newModel)[mesh] ||
            csmGetDrawableParentPartIndices(oldModel)[mesh] != csmGetDrawableParentPartIndices(newModel)[mesh] ||
            csmGetDrawableMaskCounts(oldModel)[mesh] != csmGetDrawableMaskCounts(newModel)[mesh] ||
            raw(csmGetDrawableMasks(oldModel)[mesh], csmGetDrawableMaskCounts(oldModel)[mesh]) !=
                raw(csmGetDrawableMasks(newModel)[mesh], csmGetDrawableMaskCounts(newModel)[mesh])) return "drawable identity/masks";
        const int oldCount = csmGetDrawableVertexCounts(oldModel)[mesh], newCount = csmGetDrawableVertexCounts(newModel)[mesh];
        if (mesh != target && oldCount != newCount) return "sibling vertex count";
        for (int old = 0; old < oldCount; ++old) {
            const int next = mesh == target ? oldToNew[old] : old;
            if (next < 0) continue;
            if (next >= newCount) return "mapped vertex count";
            if (raw(csmGetDrawableVertexPositions(oldModel)[mesh] + old, 1) != raw(csmGetDrawableVertexPositions(newModel)[mesh] + next, 1)) return "surviving positions";
            if (raw(csmGetDrawableVertexUvs(oldModel)[mesh] + old, 1) != raw(csmGetDrawableVertexUvs(newModel)[mesh] + next, 1)) return "surviving UV";
        }
        for (int vertex = 0; vertex < newCount; ++vertex) {
            const auto p = csmGetDrawableVertexPositions(newModel)[mesh][vertex];
            if (!std::isfinite(p.X) || !std::isfinite(p.Y)) return "nonfinite output";
        }
        if (csmGetDrawableOpacities(oldModel)[mesh] != csmGetDrawableOpacities(newModel)[mesh] ||
            csmGetDrawableDrawOrders(oldModel)[mesh] != csmGetDrawableDrawOrders(newModel)[mesh] ||
            csmGetDrawableRenderOrders(oldModel)[mesh] != csmGetDrawableRenderOrders(newModel)[mesh] ||
            raw(csmGetDrawableMultiplyColors(oldModel) + mesh, 1) != raw(csmGetDrawableMultiplyColors(newModel) + mesh, 1) ||
            raw(csmGetDrawableScreenColors(oldModel) + mesh, 1) != raw(csmGetDrawableScreenColors(newModel) + mesh, 1)) return "opacity/order/colors";
        if (mesh != target && raw(csmGetDrawableIndices(oldModel)[mesh], csmGetDrawableIndexCounts(oldModel)[mesh]) != raw(csmGetDrawableIndices(newModel)[mesh], csmGetDrawableIndexCounts(newModel)[mesh])) return "sibling triangles";
    }
    return {};
}
QByteArray texture()
{
    QImage image(128, 128, QImage::Format_RGBA8888); image.fill(QColor(61, 113, 173, 255));
    QBuffer buffer; buffer.open(QIODevice::WriteOnly); image.save(&buffer, "PNG"); return buffer.data();
}
bool textures(CModelDocument& document, QString* error)
{
    CModelMaterialState materials; if (!document.CaptureDrawableMaterials(materials, error)) return false;
    CTextureAssets::Catalog sources;
    for (const auto& material : materials) if (material.textureIndex >= 0 && !sources.contains(material.textureIndex)) {
        CTextureAssets::Source source; if (!CTextureAssets::MakeSource(QString("/tmp/alpha-core-%1.png").arg(material.textureIndex), texture(), source, error)) return false;
        sources.insert(material.textureIndex, source);
    }
    return document.InitializeTextureWorkspace(sources, {}, error);
}
bool artwork(CModelDocument& document, const QString& id, QString* error)
{
    using A=CModelArtwork; CModelDrawableMaterial current; if (!document.CaptureDrawableMaterial(id,current,error))return false;
    const auto& source=document.TextureSources()[current.textureIndex]; A::State state;
    state.images.insert(source.digest,{source.size,source.png});state.families.insert("family",{"family","图集快照",A::SourceKind::AtlasSnapshot,"revision"});
    A::Revision revision;revision.id="revision";revision.familyId="family";revision.sourceBytes=source.png;revision.sourceDigest=source.digest;revision.canvasSize=source.size;
    A::Layer layer;layer.id="layer";layer.bounds=QRect(QPoint(),source.size);layer.imageDigest=source.digest;revision.layers={layer};state.revisions.insert(revision.id,revision);
    state.materials.insert("material",{"material","","family","revision","layer",A::Boundary::Repeat,false,{}});
    A::Binding binding;binding.meshId=id;binding.placementId="placement";binding.sampledMaterial=current;
    QRectF domain(QPointF(),QSizeF(source.size));
    for(int i=0;i<current.uvs.size();i+=2){QPointF p(double(current.uvs[i])*source.size.width(),double(current.uvs[i+1])*source.size.height());binding.familyCoordinates.append(p);domain=domain.united(QRectF(p,QSizeF(.001,.001)));}
    state.placements.insert("placement",{"placement","material",document.TextureWorkspace().atlases[current.textureIndex].id,{},domain,1,A::Filter::ExactTexels});
    if(!A::TopologyDigest(binding.familyCoordinates.size(),document.DrawableIndices(id),binding.topologyDigest,error))return false;
    state.bindings.insert(id,binding);return document.SetArtwork(state,error);
}
QString formDifference(const CSectionTable& before,const CSectionTable& after,int oldRow,int newRow,const Builder::MeshEdit& edit)
{
    const auto* oldForm=before.art_mesh_keyforms[oldRow];const auto* newForm=after.art_mesh_keyforms[newRow];
    if(oldForm->opacities!=newForm->opacities||oldForm->draw_orders!=newForm->draw_orders||oldForm->multiply_color_sources_index!=newForm->multiply_color_sources_index||oldForm->screen_color_sources_index!=newForm->screen_color_sources_index)return "keyform scalars";
    for(int vertex=0;vertex<edit.vertices.size();++vertex)for(int axis=0;axis<2;++axis){const auto& w=edit.vertices[vertex];double sum=0,value=0,minimum=std::numeric_limits<double>::infinity(),maximum=-minimum;for(double x:w.weights)sum+=x;
        for(int i=0;i<w.indices.size();++i){const double old=before.keyform_positions->xys[oldForm->keyform_position_sources_begin_indices+w.indices[i]*2+axis];value+=old*(w.weights[i]/sum);minimum=std::min(minimum,old);maximum=std::max(maximum,old);}
        if(after.keyform_positions->xys[newForm->keyform_position_sources_begin_indices+vertex*2+axis]!=float(std::clamp(value,minimum,maximum)))return "keyform vertex weights";}
    return {};
}
}
class AlphaMeshCoreTests:public QObject
{
    Q_OBJECT
private slots:
    void actualAtlasDensityPresets_data()
    {
        QTest::addColumn<QString>("meshId"); QTest::addColumn<int>("density");
        for (const char* mesh : {"BrowR", "ArmR", "Face"}) for (int density = 0; density < 3; ++density)
            QTest::newRow(qPrintable(QString(mesh) + '-' + QString::number(density))) << QString(mesh) << density;
    }
    void actualAtlasDensityPresets()
    {
        QFETCH(QString, meshId); QFETCH(int, density); const QString folder = QString(TEST_RESOURCES) + "/Mark/";
        CModelDocument document; QString error; QVERIFY(document.Load(folder + "Mark.moc3", &error));
        const QString texturePath = folder + "Mark.2048/texture_00.png"; CTextureAssets::Source texture;
        QVERIFY(CTextureAssets::MakeSource(texturePath, read(texturePath), texture, &error));
        QVERIFY(document.InitializeTextureWorkspace({{0, texture}}, {}, &error));
        CAlphaMeshSource::Source source; QVERIFY2(CAlphaMeshSource::Capture(document, meshId, source, &error), qPrintable(error));
        Planner::Plan plan; QElapsedTimer timer; timer.start();
        QVERIFY2(Planner::Build(source.input, Planner::Preset(Planner::Density(density)), plan, &error), qPrintable(error));
        const auto elapsed = timer.elapsed(); QVERIFY2(Planner::Validate(source.input, plan, &error), qPrintable(error));
        qInfo().noquote() << QStringLiteral("Mark %1 density=%2: %3→%4 vertices, %5 triangles, %6 ms, %7 operations, %8 scanned pixels")
            .arg(meshId).arg(density).arg(plan.statistics.oldVertices).arg(plan.statistics.newVertices).arg(plan.statistics.triangles)
            .arg(elapsed).arg(plan.statistics.operations).arg(plan.statistics.scannedPixels);
        Builder::Request request; request.meshes = {plan.edit}; CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request, command, &error), qPrintable(error)); QVERIFY(document.ExecuteEdit(command));
        QByteArray bytes; QVERIFY(document.BuildRuntimeMoc(bytes, &error)); CoreModel model; QVERIFY(model.load(bytes));
        const int target = drawableIndex(model.value, meshId); QVERIFY(target >= 0); QCOMPARE(csmGetDrawableVertexCounts(model.value)[target], plan.edit.vertices.size());
        QVERIFY(model.intact());
    }
    void generatedTopologyPreservesAuthoringAndCore_data()
    {
        QTest::addColumn<QString>("path");
        for(const char* name:{"Mark","Natori","Rice","Wanko"})QTest::newRow(name)<<QString(TEST_RESOURCES)+'/'+name+'/'+name+".moc3";
        const auto extra=qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");if(!extra.isEmpty())QTest::newRow("BlendShape")<<extra;
    }
    void generatedTopologyPreservesAuthoringAndCore()
    {
        QFETCH(QString,path);const auto source=read(path);CModelDocument document;QString error;QVERIFY2(document.LoadBytes(source,path,&error),qPrintable(error));QVERIFY2(textures(document,&error),qPrintable(error));
        auto reader=CReader::FromBytes(source);CSectionTable original(reader);QVERIFY(original.IsValid());QSet<int> blendTargets;
        for(const auto* shape:original.blend_shapes_art_meshes)if(shape->blend_shape_keyform_binding_sources_counts)blendTargets.insert(shape->target_indices);
        Planner::Plan plan;CAlphaMeshSource::Source prepared;int target=-1;Planner::Settings settings=Planner::Preset(Planner::Density::Fine);settings.margin=0;
        for(int i=0;i<original.art_meshes.size();++i){if(!blendTargets.isEmpty()&&!blendTargets.contains(i))continue;
            CAlphaMeshSource::Source candidate;if(!CAlphaMeshSource::Capture(document,original.art_meshes[i]->ids,candidate,&error))continue;
            if(Planner::Build(candidate.input,settings,plan,&error)&&plan.edit.vertices.size()>original.art_meshes[i]->vertex_counts){target=i;prepared=std::move(candidate);break;}}
        QVERIFY2(target>=0,qPrintable("No generatable real mesh: "+error));const auto id=prepared.input.artMeshId;
        QVERIFY2(artwork(document,id,&error),qPrintable(error));QVERIFY(CAlphaMeshSource::Capture(document,id,prepared,&error));QVERIFY2(Planner::Build(prepared.input,settings,plan,&error),qPrintable(error));QVERIFY(Planner::Validate(prepared.input,plan,&error));
        const auto beforeArtwork=document.Artwork();const auto beforeState=document.AuthoringStructureState();Builder::Request request;request.meshes={plan.edit};CModelEdit command;
        QVERIFY2(document.MakeMeshStructureEdit(request,command,&error),qPrintable(error));QVERIFY(document.ExecuteEdit(command));QVERIFY(document.CanUndoEdit());
        QByteArray changed;QVERIFY(document.BuildPreviewMoc(changed,&error));CoreModel baseline,modified;QVERIFY(baseline.load(source));QVERIFY(modified.load(changed));const int index=drawableIndex(baseline.value,id);QVERIFY(index>=0);
        double maximumNewPointDistance=0;
        for(const auto& pose:poses(baseline.value)){baseline.pose(pose);modified.pose(pose);const auto difference=unchangedVertices(baseline.value,modified.value,index,plan.edit.oldToNew);QVERIFY2(difference.isEmpty(),qPrintable(difference));
            for(int vertex=0;vertex<plan.edit.vertices.size();++vertex){const auto& weights=plan.edit.vertices[vertex];if(weights.indices.size()==1&&weights.weights==QVector<double>{1})continue;
                QPointF oldSurface;double total=0;for(double weight:weights.weights)total+=weight;
                for(int i=0;i<weights.indices.size();++i){const auto point=csmGetDrawableVertexPositions(baseline.value)[index][weights.indices[i]];oldSurface+=QPointF(point.X,point.Y)*(weights.weights[i]/total);}
                const auto actual=csmGetDrawableVertexPositions(modified.value)[index][vertex];maximumNewPointDistance=std::max(maximumNewPointDistance,QLineF(oldSurface,QPointF(actual.X,actual.Y)).length());}}
        qInfo().noquote()<<QStringLiteral("%1 / %2：新增点与旧表面重心的最大 Core 距离 %3；仅记录，不冒称非线性变形保持逐点等价")
            .arg(QFileInfo(path).fileName(),id).arg(maximumNewPointDistance,0,'g',12);
        auto nextReader=CReader::FromBytes(changed);CSectionTable next(nextReader);QVERIFY(next.IsValid());const auto* old=original.art_meshes[target];const auto* now=next.art_meshes[target];QCOMPARE(now->keyform_sources_counts,old->keyform_sources_counts);
        for(int row=0;row<old->keyform_sources_counts;++row){const auto difference=formDifference(original,next,old->keyform_sources_begin_indices+row,now->keyform_sources_begin_indices+row,plan.edit);QVERIFY2(difference.isEmpty(),qPrintable(difference));}
        int blendForms=0;for(int shape=0;shape<original.blend_shapes_art_meshes.size();++shape){const auto* a=original.blend_shapes_art_meshes[shape];if(a->target_indices!=target)continue;const auto* b=next.blend_shapes_art_meshes[shape];
            QCOMPARE(a->blend_shape_keyform_binding_sources_counts,b->blend_shape_keyform_binding_sources_counts);for(int binding=0;binding<a->blend_shape_keyform_binding_sources_counts;++binding){const auto* first=original.blend_shape_keyform_bindings[a->blend_shape_keyform_binding_sources_begin_indices+binding];const auto* last=next.blend_shape_keyform_bindings[b->blend_shape_keyform_binding_sources_begin_indices+binding];QCOMPARE(first->keyform_sources_blend_shape_counts,last->keyform_sources_blend_shape_counts);
                for(int row=0;row<first->keyform_sources_blend_shape_counts;++row){++blendForms;const auto difference=formDifference(original,next,first->keyform_sources_blend_shape_indices+row,last->keyform_sources_blend_shape_indices+row,plan.edit);QVERIFY2(difference.isEmpty(),qPrintable(difference));}}}
        if(!blendTargets.isEmpty())QVERIFY(blendForms>0);QVERIFY(document.ValidateArtworkReferences(document.Artwork(),&error));
        const auto& oldQ=beforeArtwork.bindings[id].familyCoordinates;const auto& newQ=document.Artwork().bindings[id].familyCoordinates;QCOMPARE(newQ.size(),plan.edit.vertices.size());
        for(int i=0;i<newQ.size();++i){QPointF expected;const auto& w=plan.edit.vertices[i];double sum=0;for(double v:w.weights)sum+=v;for(int j=0;j<w.indices.size();++j)expected+=oldQ[w.indices[j]]*(w.weights[j]/sum);QVERIFY(QLineF(expected,newQ[i]).length()<1e-10);}
        CModelSession session;session.version=12;session.meshStructure=true;session.sourceMoc=path;session.sourceDigest=document.SourceDigest();session.structure=document.ParameterStructure();session.artwork=document.Artwork();session.materialStructure=document.HasMaterialStructure();session.drawableMaterials=document.DrawableMaterialOverrides();
        CTextureWorkspace::Draft draft;QVERIFY(CTextureWorkspace::CaptureDraft(document.TextureSources(),document.TextureWorkspace(),draft,&error));session.textureWorkspace=draft;
        CModelSession decoded;QVERIFY2(CModelSession::FromJson(session.ToJson(),&decoded,&error),qPrintable(error));CModelDocument restored;QVERIFY2(decoded.RestoreDocument(document,restored,&error),qPrintable(error));QByteArray restoredBytes;QVERIFY(restored.BuildPreviewMoc(restoredBytes,&error));QCOMPARE(restoredBytes,changed);QCOMPARE(restored.Artwork(),document.Artwork());
        QVERIFY(document.UndoEdit());QCOMPARE(document.AuthoringStructureState(),beforeState);QCOMPARE(document.Artwork(),beforeArtwork);QVERIFY(document.RedoEdit());QByteArray redone;QVERIFY(document.BuildPreviewMoc(redone,&error));QCOMPARE(redone,changed);QCOMPARE(document.SourceBytes(),source);QVERIFY(baseline.intact());QVERIFY(modified.intact());
    }
    void generatedPlanCannotDropGluePinsSilently()
    {
        CModelDocument document;QString error;QVERIFY(document.Load(QString(TEST_RESOURCES)+"/Natori/Natori.moc3",&error));QVERIFY(textures(document,&error));auto reader=CReader::FromBytes(document.SourceBytes());CSectionTable table(reader);QVERIFY(!table.glue.isEmpty());const auto* glue=table.glue.first();const int target=glue->art_mesh_indices_a;
        CAlphaMeshSource::Source prepared;QVERIFY(CAlphaMeshSource::Capture(document,table.art_meshes[target]->ids,prepared,&error));QVERIFY(!prepared.input.pinnedVertices.isEmpty());
        Builder::MeshEdit edit;edit.artMeshId=prepared.input.artMeshId;const int dropped=prepared.input.pinnedVertices.first();const int count=prepared.input.pixelCoordinates.size();
        for(int i=0;i<count;++i){edit.oldToNew.append(i==dropped?-1:edit.vertices.size());if(i!=dropped)edit.vertices.append({{i},{1}});}
        for(int triangle=0;triangle<prepared.input.indices.size();triangle+=3){const auto a=prepared.input.indices[triangle],b=prepared.input.indices[triangle+1],c=prepared.input.indices[triangle+2];if(a==dropped||b==dropped||c==dropped)continue;edit.indices<<quint16(edit.oldToNew[a])<<quint16(edit.oldToNew[b])<<quint16(edit.oldToNew[c]);}
        Builder::Request request;request.meshes={edit};const auto before=document.AuthoringStructureState();CModelEdit command;QVERIFY(!document.MakeMeshStructureEdit(request,command,&error));QVERIFY(error.contains("Glue"));QCOMPARE(document.AuthoringStructureState(),before);
    }
};
QTEST_GUILESS_MAIN(AlphaMeshCoreTests)
#include "alpha_mesh_core_tests.moc"
