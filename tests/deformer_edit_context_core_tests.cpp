#include "deformer_edit_context_fixture.h"
#include "live2dstruct/ckeyformeditcontroller.h"
#include "live2dstruct/ckeyformgridsnapshot.h"
#include "live2dstruct/ckeyformmappingcontext.h"
#include "live2dstruct/cblendshapeposeevaluator.h"
#include "live2dstruct/tree/c2dtree.h"
#include "live2dstruct/tree/crotationnode.h"

class DeformerEditContextCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void officialMaoPartialWarpDrag_data()
    {
        QTest::addColumn<QString>("target"); QTest::addColumn<int>("mode");
        for (const QString target : {QString("Warp133"), QString("Warp131")})
            for (int mode : {0, 1, 2, 3, 4})
                QTest::newRow(qPrintable(target + '-' + QString::number(mode))) << target << mode;
    }

    void officialMaoPartialWarpDrag()
    {
        QFETCH(QString, target); QFETCH(int, mode);
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to official Mao");
        const auto bytes = read(path);
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        int specific = -1;
        for (const auto* deformer : table.deformers) if (deformer->ids == target) specific = deformer->specific_sources_indices;
        QVERIFY(specific >= 0);
        QString parameterId; float key = 0;
        for (const auto* shape : table.blend_shapes_warp_deformers) {
            if (shape->target_indices != specific || !shape->blend_shape_keyform_binding_sources_counts) continue;
            const auto* binding = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices];
            const int index = binding->parameter_binding_sources_indices;
            const auto* parameterBinding = table.blend_shape_parameter_bindings[index];
            if (parameterBinding->keys_sources_counts < 2) continue;
            for (const auto* parameter : table.parameters) {
                if (parameter->parameter_types != 1 || index < parameter->blend_shape_parameter_binding_sources_begin_indices ||
                    index >= parameter->blend_shape_parameter_binding_sources_begin_indices + parameter->blend_shape_parameter_binding_sources_counts) continue;
                parameterId = parameter->ids;
                const int keyIndex = parameterBinding->base_key_indices == 0 ? parameterBinding->keys_sources_counts - 1 : 0;
                key = table.keys->GetData(parameterBinding->keys_sources_begin_indices + keyIndex); break;
            }
            if (!parameterId.isEmpty()) break;
        }
        QVERIFY(!parameterId.isEmpty()); QMap<QString,float> pose;
        for (const auto& parameter : document.Parameters()) pose[parameter.id] = parameter.defaultValue;
        pose[parameterId] = key;
        CBlendShapePoseEvaluator evaluator; CBlendShapePoseEvaluator::Overrides overrides;
        QVERIFY(evaluator.Build(table, pose, overrides, &error));
        QVERIFY(std::any_of(overrides.warpPositions[target].cbegin(), overrides.warpPositions[target].cend(), [](float v) {return v != 0;}));
        const auto before = document.DeformerWarpKeyforms(target); QVERIFY(!before.isEmpty());
        CDeformerEditContext mapping; QVERIFY2(mapping.Build(document,target,"warp",0,pose,&error),qPrintable(error));
        const int count = before[0].positions.size()/2; QVERIFY(count > 3);
        QVector<int> selected = mode == 4 ? QVector<int>{count-1,0} : QVector<int>{count-1};
        QVector<double> weights(count,0);
        for (int vertex : selected) weights[vertex] = mode == 1 ? .25 : 1;
        if (mode == 2) {
            selected.clear(); for (int vertex=count-1; vertex>=0; --vertex) { selected.append(vertex); weights[vertex]=1; }
        }
        const QPointF delta(.007, -.004);
        auto desired = mapping.ModelPoints();
        for (int vertex : selected) {desired[vertex*2] += float(delta.x()*weights[vertex]);desired[vertex*2+1] += float(delta.y()*weights[vertex]);}
        QVector<float> oracleLocal; QVERIFY2(mapping.FromModel(desired,oracleLocal,&error),qPrintable(error));
        auto expected = before;
        for (int vertex : selected) {expected[0].positions[vertex*2]=oracleLocal[vertex*2];expected[0].positions[vertex*2+1]=oracleLocal[vertex*2+1];}
        CKeyformEditController controller; controller.SetDocument(&document);
        bool reject = false; QByteArray published; QMap<QString,float> publishedPose;
        controller.SetPublisher([&](const CModelDocument& candidate,const QMap<QString,float>& values,bool editing,QString* reason) {
            if (reject) { *reason="controlled rejection";return false; }
            if (!editing || !candidate.BuildPreviewMoc(published,reason)) return false;
            publishedPose=values;return true;
        },[](QString*) {return true;});
        CKeyformBindingGrid strict;
        QVERIFY(strict.Build(table, &error));
        QMap<QString, float> corner, snapshotCorner;
        QVERIFY(!strict.Pose(CKeyformBindingGrid::Warp, target, 0, corner, &error));
        QVERIFY2(document.KeyformParameterValues(target, "warp", 0, corner, &error), qPrintable(error));
        QVERIFY(!corner.contains(parameterId));
        CKeyformGridSnapshot snapshot;
        QVERIFY(document.DescribeKeyformGrid(target, "warp", snapshot, &error));
        QVERIFY(snapshot.diagnostic.isEmpty());
        QVERIFY2(snapshot.Pose(0, snapshotCorner, &error), qPrintable(error));
        QCOMPARE(snapshotCorner, corner);
        QVERIFY(!snapshot.Pose(snapshot.keyformCount, snapshotCorner, &error));
        QCOMPARE(snapshotCorner, corner);
        QVERIFY2(controller.Activate(target,"warp",0,pose,&error),qPrintable(error));
        QCOMPARE(controller.Pose().value(parameterId),key);
        const auto baselinePublished=published; const auto revision=document.Revision();
        if (mode == 1) QVERIFY2(controller.BeginWeightedWarpDrag(weights,&error),qPrintable(error));
        else QVERIFY2(controller.BeginDeformerDrag(CKeyformEditController::DeformerHandle::WarpPoints,selected,&error),qPrintable(error));
        reject = mode == 3;
        if (reject) {
            QVERIFY(!controller.MoveDeformerDrag(delta,0,&error));
            QCOMPARE(document.DeformerWarpKeyforms(target),before);QCOMPARE(document.Revision(),revision);QCOMPARE(published,baselinePublished);
            reject=false;
        }
        QVERIFY2(controller.MoveDeformerDrag(delta,0,&error),qPrintable(error));
        const auto actual=document.DeformerWarpKeyforms(target);
        QVERIFY(near(actual[0].positions,expected[0].positions));
        for (int vertex=0;vertex<count;++vertex) if (!selected.contains(vertex)) {
            QCOMPARE(actual[0].positions[vertex*2],before[0].positions[vertex*2]);
            QCOMPARE(actual[0].positions[vertex*2+1],before[0].positions[vertex*2+1]);
        }
        for (int form=1;form<actual.size();++form) QCOMPARE(actual[form],before[form]);
        QCOMPARE(actual[0].opacity,before[0].opacity);
        CDeformerEditContext changed; QVERIFY2(changed.Build(document,target,"warp",0,pose,&error),qPrintable(error));
        for (int vertex : selected) QVERIFY(near(changed.ModelPoints().mid(vertex*2,2),desired.mid(vertex*2,2)));
        QByteArray oracle=bytes; const auto* warp=table.warp_deformers[specific];
        const int begin=table.warp_deformer_keyforms[warp->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
        for (int vertex : selected) for (int axis=0;axis<2;++axis)
            real(oracle,table.sectionOffsetTable->keyform_positions->xys+(begin+vertex*2+axis)*4,expected[0].positions[vertex*2+axis]);
        CoreModel reference, edited; QVERIFY(reference.load(oracle)); QVERIFY(edited.load(published));
        setPose(reference,pose);setPose(edited,pose);
        QVERIFY2(difference(frame(reference.value),frame(edited.value)).isEmpty(),qPrintable(difference(frame(reference.value),frame(edited.value))));
        QVERIFY(reference.intact());QVERIFY(edited.intact());
        controller.FinishDrag(); CModelEdit edit;
        QVERIFY2(controller.History(true,edit,&error),qPrintable(error));QCOMPARE(document.DeformerWarpKeyforms(target),before);
        QVERIFY2(controller.History(false,edit,&error),qPrintable(error));QCOMPARE(document.DeformerWarpKeyforms(target),actual);
        QVERIFY2(controller.BeginDeformerDrag(CKeyformEditController::DeformerHandle::WarpPoints,{count-1},&error),qPrintable(error));
        QVERIFY2(controller.MoveDeformerDrag(delta,0,&error),qPrintable(error));
        QVERIFY2(controller.CancelDrag(&error),qPrintable(error));QCOMPARE(document.DeformerWarpKeyforms(target),actual);
    }

    void officialMaoRotationControllerDrag_data()
    {
        QTest::addColumn<QString>("target");
        QTest::addColumn<bool>("angular");
        for (const QString target : {QString("Rotation3"), QString("Rotation64")})
            for (bool angular : {false, true})
                QTest::newRow(qPrintable(target + (angular ? "-angle" : "-origin"))) << target << angular;
    }

    void officialMaoRotationControllerDrag()
    {
        QFETCH(QString, target); QFETCH(bool, angular);
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to official Mao");
        const auto bytes = read(path);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        int specific = -1;
        for (const auto* deformer : table.deformers)
            if (deformer->ids == target && deformer->types == 1) specific = deformer->specific_sources_indices;
        QVERIFY(specific >= 0);
        QString parameterId; float key = 0;
        for (const auto* shape : table.blend_shapes_rotation_deformers) {
            if (shape->target_indices != specific || !shape->blend_shape_keyform_binding_sources_counts) continue;
            const auto* binding = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices];
            const int index = binding->parameter_binding_sources_indices;
            const auto* parameterBinding = table.blend_shape_parameter_bindings[index];
            if (parameterBinding->keys_sources_counts < 2) continue;
            for (const auto* parameter : table.parameters) {
                if (parameter->parameter_types != 1 || index < parameter->blend_shape_parameter_binding_sources_begin_indices ||
                    index >= parameter->blend_shape_parameter_binding_sources_begin_indices + parameter->blend_shape_parameter_binding_sources_counts) continue;
                parameterId = parameter->ids;
                const int keyIndex = parameterBinding->base_key_indices == 0 ? parameterBinding->keys_sources_counts - 1 : 0;
                key = table.keys->GetData(parameterBinding->keys_sources_begin_indices + keyIndex);
                break;
            }
            if (!parameterId.isEmpty()) break;
        }
        QVERIFY(!parameterId.isEmpty());
        QMap<QString, float> pose;
        for (const auto& parameter : document.Parameters()) pose[parameter.id] = parameter.defaultValue;
        pose[parameterId] = key;
        CBlendShapePoseEvaluator::Overrides overrides;
        QVERIFY2(CBlendShapePoseEvaluator().Build(table, pose, overrides, &error), qPrintable(error));
        const auto blend = overrides.rotations.value(target);
        QVERIFY(!blend.reflectionConflict);
        if (target == "Rotation3") QVERIFY(blend.angle != 0 || blend.originX != 0 || blend.originY != 0 || blend.scale != 0);
        const auto before = document.DeformerRotationKeyforms(target);
        QVERIFY(!before.isEmpty());
        CDeformerEditContext mapping;
        QVERIFY2(mapping.Build(document, target, "rotation", 0, pose, &error), qPrintable(error));
        const QPointF delta = angular ? QPointF() : QPointF(.005, -.003);
        const double angleDelta = angular ? 7.0 : 0.0;
        auto expected = before;
        if (angular) expected[0].angle = float(double(before[0].angle) + angleDelta);
        else {
            auto desired = mapping.ModelPoints();
            desired[0] = float(double(desired[0]) + delta.x());
            desired[1] = float(double(desired[1]) + delta.y());
            QVector<float> local;
            QVERIFY2(mapping.FromModel(desired, local, &error), qPrintable(error));
            QCOMPARE(local.size(), 2);
            expected[0].originX = local[0]; expected[0].originY = local[1];
        }
        CKeyformEditController controller; controller.SetDocument(&document);
        QByteArray published; QMap<QString, float> publishedPose; bool reject = false;
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>& values, bool editing, QString* reason) {
            if (reject) { if (reason) *reason = "controlled rejection"; return false; }
            if (!editing || !candidate.BuildPreviewMoc(published, reason)) return false;
            publishedPose = values; return true;
        }, [](QString*) { return true; });
        QVERIFY2(controller.Activate(target, "rotation", 0, pose, &error), qPrintable(error));
        QCOMPARE(controller.Pose().value(parameterId), key);
        const auto baselinePublished = published; const auto revision = document.Revision();
        const auto handle = angular ? CKeyformEditController::DeformerHandle::RotationAngle : CKeyformEditController::DeformerHandle::RotationOrigin;
        QVERIFY2(controller.BeginDeformerDrag(handle, {}, &error), qPrintable(error));
        reject = true;
        QVERIFY(!controller.MoveDeformerDrag(delta, angleDelta, &error));
        QCOMPARE(document.DeformerRotationKeyforms(target), before);
        QCOMPARE(document.Revision(), revision); QCOMPARE(published, baselinePublished);
        reject = false;
        QVERIFY2(controller.MoveDeformerDrag(delta, angleDelta, &error), qPrintable(error));
        const auto actual = document.DeformerRotationKeyforms(target);
        QCOMPARE(actual, expected); QCOMPARE(publishedPose, controller.Pose());
        QByteArray oracle = bytes;
        const int row = table.rotation_deformers[specific]->keyform_sources_begin_indices;
        const auto& columns = *table.sectionOffsetTable->rotation_deformer_keyforms;
        real(oracle, columns.angles + row * 4, expected[0].angle);
        real(oracle, columns.origin_x + row * 4, expected[0].originX);
        real(oracle, columns.origin_y + row * 4, expected[0].originY);
        CoreModel reference, edited;
        QVERIFY(reference.load(oracle)); QVERIFY(edited.load(published));
        setPose(reference, publishedPose); setPose(edited, publishedPose);
        QVERIFY2(difference(frame(reference.value), frame(edited.value)).isEmpty(),
                 qPrintable(difference(frame(reference.value), frame(edited.value))));
        QVERIFY(reference.intact()); QVERIFY(edited.intact());
        controller.FinishDrag(); CModelEdit command;
        QVERIFY2(controller.History(true, command, &error), qPrintable(error));
        QCOMPARE(document.DeformerRotationKeyforms(target), before);
        QVERIFY2(controller.History(false, command, &error), qPrintable(error));
        QCOMPARE(document.DeformerRotationKeyforms(target), actual);
        QVERIFY2(controller.BeginDeformerDrag(handle, {}, &error), qPrintable(error));
        QVERIFY2(controller.MoveDeformerDrag(delta, angleDelta, &error), qPrintable(error));
        QVERIFY2(controller.CancelDrag(&error), qPrintable(error));
        QCOMPARE(document.DeformerRotationKeyforms(target), actual);
    }

    void officialMaoArtMeshControllerDrag_data()
    {
        QTest::addColumn<float>("blendValue");
        for (float value : {0.3f, 1.0f}) QTest::newRow(qPrintable(QString::number(value))) << value;
    }

    void officialMaoArtMeshControllerDrag()
    {
        QFETCH(float, blendValue);
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC to official Mao");
        const auto bytes = read(path);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(bytes, path, &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader); QVERIFY(table.IsValid());
        const QString target = "ArtMesh263", parameterId = "ParamMouthAngryLine";
        QMap<QString, float> pose;
        for (const auto& parameter : document.Parameters()) pose[parameter.id] = parameter.defaultValue;
        QVERIFY(pose.contains(parameterId)); pose[parameterId] = blendValue;
        CKeyformEditController controller; controller.SetDocument(&document);
        QByteArray published; QMap<QString, float> publishedPose; bool reject = false;
        controller.SetPublisher([&](const CModelDocument& candidate, const QMap<QString, float>& values, bool editing, QString* reason) {
            if (reject) { if (reason) *reason = "controlled rejection"; return false; }
            if (!editing || !candidate.BuildPreviewMoc(published, reason)) return false;
            publishedPose = values; return true;
        }, [](QString*) { return true; });
        QVERIFY2(controller.Activate(target, "artmesh", 0, pose, &error), qPrintable(error));
        QCOMPARE(controller.Pose().value(parameterId), blendValue);
        pose = controller.Pose();
        CBlendShapePoseEvaluator::Overrides overrides;
        QVERIFY2(CBlendShapePoseEvaluator().Build(table, pose, overrides, &error), qPrintable(error));
        const auto blend = overrides.artMeshPositions.value(target);
        QVERIFY(std::any_of(blend.cbegin(), blend.cend(), [](float value) { return value != 0; }));
        CKeyformMappingContext mapping;
        QVERIFY2(mapping.Build(document, target, 0, pose, &error), qPrintable(error));
        const auto before = document.DrawableKeyformPositions(target, 0);
        QVERIFY(before.size() > 6);
        QVector<float> model;
        QVERIFY2(mapping.ToModel(before, model, &error), qPrintable(error));
        CoreModel baseline; QVERIFY(baseline.load(published)); setPose(baseline, pose);
        int meshIndex = -1;
        for (int index = 0; index < csmGetDrawableCount(baseline.value); ++index)
            if (QString::fromUtf8(csmGetDrawableIds(baseline.value)[index]) == target) meshIndex = index;
        QVERIFY(meshIndex >= 0);
        QVector<float> runtime;
        const auto* vertices = csmGetDrawableVertexPositions(baseline.value)[meshIndex];
        for (int index = 0; index < csmGetDrawableVertexCounts(baseline.value)[meshIndex]; ++index)
            runtime << vertices[index].X << vertices[index].Y;
        const QVector<int> selected{int(before.size() / 2) - 1, 0};
        const QPointF delta(.003, -.002);
        auto desired = model;
        for (int vertex : selected) {
            desired[vertex * 2] = float(double(desired[vertex * 2]) + delta.x());
            desired[vertex * 2 + 1] = float(double(desired[vertex * 2 + 1]) + delta.y());
        }
        QVector<float> inverse;
        QVERIFY2(mapping.FromModel(desired, inverse, &error), qPrintable(error));
        auto expected = before;
        for (int vertex : selected) { expected[vertex * 2] = inverse[vertex * 2]; expected[vertex * 2 + 1] = inverse[vertex * 2 + 1]; }
        QVERIFY2(controller.BeginDrag(selected, runtime, &error), qPrintable(error));
        const auto revision = document.Revision(); const auto baselinePublished = published;
        reject = true; QVERIFY(!controller.DragBy(delta, &error));
        QCOMPARE(document.DrawableKeyformPositions(target, 0), before);
        QCOMPARE(document.Revision(), revision); QCOMPARE(published, baselinePublished);
        reject = false; QVERIFY2(controller.DragBy(delta, &error), qPrintable(error));
        const auto actual = document.DrawableKeyformPositions(target, 0);
        QCOMPARE(actual, expected);
        QByteArray oracle = bytes;
        const CArt_meshes* mesh = nullptr;
        for (const auto* source : table.art_meshes) if (source->ids == target) mesh = source;
        QVERIFY(mesh);
        const int begin = table.art_mesh_keyforms[mesh->keyform_sources_begin_indices]->keyform_position_sources_begin_indices;
        for (int vertex : selected) for (int axis = 0; axis < 2; ++axis)
            real(oracle, table.sectionOffsetTable->keyform_positions->xys + (begin + vertex * 2 + axis) * 4, expected[vertex * 2 + axis]);
        CoreModel reference, edited; QVERIFY(reference.load(oracle)); QVERIFY(edited.load(published));
        setPose(reference, publishedPose); setPose(edited, publishedPose);
        QVERIFY2(difference(frame(reference.value), frame(edited.value)).isEmpty(),
                 qPrintable(difference(frame(reference.value), frame(edited.value))));
        QVERIFY(reference.intact()); QVERIFY(edited.intact());
        controller.FinishDrag(); CModelEdit command;
        QVERIFY2(controller.History(true, command, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions(target, 0), before);
        QVERIFY2(controller.History(false, command, &error), qPrintable(error));
        QCOMPARE(document.DrawableKeyformPositions(target, 0), actual);
    }

    void witnessGeometryMatchesCore_data()
    {
        QTest::addColumn<QString>("type"); QTest::addColumn<QString>("parent"); QTest::addColumn<float>("scale"); QTest::addColumn<int>("reflections");
        for (const QString type : {QString("warp"), QString("rotation")}) {
            const QStringList parents = type == "warp" ? QStringList{"root", "rotation", "folded"} : QStringList{"root", "rotation", "warp", "nested"};
            for (const auto& parent : parents) for (float scale : {-0.5f, 0.0f, 0.5f}) for (int reflect : {0, 1, 2, 3})
                QTest::newRow(qPrintable(type + '-' + parent + '-' + QString::number(scale) + '-' + QString::number(reflect))) << type << parent << scale << reflect;
        }
    }
    void witnessGeometryMatchesCore()
    {
        QFETCH(QString, type); QFETCH(QString, parent); QFETCH(float, scale); QFETCH(int, reflections);
        const auto source = fixture(type, parent, scale, reflections); QVERIFY(!source.bytes.isEmpty());
        CModelDocument document; QString error; QVERIFY2(document.LoadBytes(source.bytes, "witness.moc3", &error), qPrintable(error));
        const auto pose = targetPose(document, source); QVERIFY(!pose.isEmpty());
        CDeformerEditContext context;
        QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        CoreModel core; QVERIFY(core.load(source.bytes)); setPose(core, pose); QVERIFY(core.intact());
        const auto points = witnessPoints(core, source, type == "warp" ? source.localPoints.size() / 2 : 3);
        const auto expected = type == "warp" ? points : points.mid(0, 2);
        QVERIFY2(near(context.ModelPoints(), expected), qPrintable(QString("actual %1,%2 vs %3,%4").arg(context.ModelPoints().value(0), 0, 'g', 9).arg(context.ModelPoints().value(1), 0, 'g', 9).arg(expected.value(0), 0, 'g', 9).arg(expected.value(1), 0, 'g', 9)));
        QVector<float> mapped, restored;
        QVERIFY(context.ToModel(source.localPoints, mapped)); QVERIFY(near(mapped, expected));
        QVERIFY2(context.FromModel(expected.mid(0, 2), restored, &error), qPrintable(error));
        QVERIFY(near(restored, source.localPoints.mid(0, 2)));
        if (type == "rotation" && scale != 0) {
            QPointF direction(points[0] - points[4], points[1] - points[5]);
            direction /= std::hypot(direction.x(), direction.y());
            QVERIFY(std::hypot(direction.x() - context.RotationDirection().x(), direction.y() - context.RotationDirection().y()) < 0.00002);
        }
        if (type == "rotation" && scale == 0) {
            QVERIFY(near(points.mid(2, 2), expected)); QVERIFY(near(points.mid(4, 2), expected));
            const auto direction = context.RotationDirection(); QVERIFY(std::abs(std::hypot(direction.x(), direction.y()) - 1) < 0.000001);
        }
    }
    void originInverseAndAngleDeltaMatchNewCoreCandidate_data()
    {
        QTest::addColumn<QString>("parent");
        for (const char* parent : {"root", "rotation", "warp", "nested"}) QTest::newRow(parent) << QString::fromLatin1(parent);
    }
    void originInverseAndAngleDeltaMatchNewCoreCandidate()
    {
        QFETCH(QString, parent);
        const auto source = fixture("rotation", parent, -0.5f, 1);
        CModelDocument document; QString error; QVERIFY(document.LoadBytes(source.bytes, "move.moc3"));
        const auto pose = targetPose(document, source);
        CDeformerEditContext context; QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        QVector<float> clicked; QVERIFY(context.ToModel({0.35f, 0.42f}, clicked));
        QVector<float> local; QVERIFY(context.FromModel(clicked, local));
        auto reader = CReader::FromBytes(source.bytes); CSectionTable table(reader);
        QByteArray movedBytes = source.bytes;
        const auto* rotation = table.rotation_deformers[table.deformers[source.targetIndex]->specific_sources_indices];
        const auto* offsets = table.sectionOffsetTable->rotation_deformer_keyforms;
        for (int i = 0; i < rotation->keyform_sources_counts; ++i) {
            const int row = rotation->keyform_sources_begin_indices + i;
            real(movedBytes, offsets->origin_x + row * 4, local[0]); real(movedBytes, offsets->origin_y + row * 4, local[1]);
        }
        CoreModel moved; QVERIFY(moved.load(movedBytes)); setPose(moved, pose);
        const auto before = witnessPoints(moved, source, 3); QVERIFY(near(before.mid(0, 2), clicked));
        QByteArray rotatedBytes = movedBytes;
        constexpr double degrees = 47;
        for (int i = 0; i < rotation->keyform_sources_counts; ++i) {
            const int row = rotation->keyform_sources_begin_indices + i;
            real(rotatedBytes, offsets->angles + row * 4, 31 + degrees);
        }
        CoreModel rotated; QVERIFY(rotated.load(rotatedBytes)); setPose(rotated, pose);
        const auto after = witnessPoints(rotated, source, 3);
        const double radians = -degrees * std::acos(-1.0) / 180;
        QVector<float> expected = before;
        for (int vertex = 1; vertex < 3; ++vertex) {
            const double x = before[vertex * 2] - before[0], y = before[vertex * 2 + 1] - before[1];
            expected[vertex * 2] = before[0] + std::cos(radians) * x - std::sin(radians) * y;
            expected[vertex * 2 + 1] = before[1] + std::sin(radians) * x + std::cos(radians) * y;
        }
        QVERIFY(near(after, expected)); QVERIFY(moved.intact()); QVERIFY(rotated.intact());
    }

    void officialMaoBlendShapeDeformerContextsRoundTrip()
    {
        const QString path = qEnvironmentVariable("LIVE2D_REAL_CORE_EXTRA_MOC");
        if (path.isEmpty()) QSKIP("Set LIVE2D_REAL_CORE_EXTRA_MOC");
        const auto bytes = read(path);
        CModelDocument document; QString error;
        QVERIFY2(document.LoadBytes(bytes, "mao.moc3", &error), qPrintable(error));
        auto reader = CReader::FromBytes(bytes); CSectionTable table(reader);
        QVERIFY(table.IsValid());
        const QStringList targets{"Warp133", "Warp131", "Rotation3", "Rotation64"};
        bool rotationDeltaObserved = false;
        for (const auto& target : targets) {
            int specific = -1, type = -1;
            for (const auto* deformer : table.deformers)
                if (deformer && deformer->ids == target) { specific = deformer->specific_sources_indices; type = deformer->types; break; }
            QVERIFY2(specific >= 0 && (type == 0 || type == 1), qPrintable(target));
            QString parameterId; float key = 0;
            const auto choose = [&](const auto& shapes) {
                for (const auto* shape : shapes) {
                    if (!shape || shape->target_indices != specific || !shape->blend_shape_keyform_binding_sources_counts) continue;
                    const auto* binding = table.blend_shape_keyform_bindings[shape->blend_shape_keyform_binding_sources_begin_indices];
                    if (!binding) continue;
                    const int bindingIndex = binding->parameter_binding_sources_indices;
                    const auto* parameterBinding = table.blend_shape_parameter_bindings.value(bindingIndex);
                    if (!parameterBinding || parameterBinding->keys_sources_counts < 2) continue;
                    for (const auto* parameter : table.parameters) {
                        if (!parameter || parameter->parameter_types != 1 ||
                            bindingIndex < parameter->blend_shape_parameter_binding_sources_begin_indices ||
                            bindingIndex >= parameter->blend_shape_parameter_binding_sources_begin_indices +
                                parameter->blend_shape_parameter_binding_sources_counts) continue;
                        parameterId = parameter->ids;
                        const int keyIndex = parameterBinding->base_key_indices == 0 ? parameterBinding->keys_sources_counts - 1 : 0;
                        key = table.keys->GetData(parameterBinding->keys_sources_begin_indices + keyIndex);
                        return;
                    }
                }
            };
            if (type == 0) choose(table.blend_shapes_warp_deformers);
            else choose(table.blend_shapes_rotation_deformers);
            QVERIFY2(!parameterId.isEmpty(), qPrintable(target));
            QMap<QString, float> pose;
            for (const auto* parameter : table.parameters) pose.insert(parameter->ids, parameter->default_values);
            pose[parameterId] = key;

            CBlendShapePoseEvaluator::Overrides overrides;
            CBlendShapePoseEvaluator evaluator;
            QVERIFY2(evaluator.Build(table, pose, overrides, &error), qPrintable(error));
            if (type == 0) {
                const auto delta = overrides.warpPositions.value(target);
                QVERIFY(!delta.isEmpty());
                QVERIFY(std::any_of(delta.cbegin(), delta.cend(), [](float value) { return value != 0.0f; }));
            } else {
                const auto delta = overrides.rotations.value(target);
                QVERIFY(!delta.reflectionConflict);
                rotationDeltaObserved |= delta.angle != 0.0f || delta.originX != 0.0f || delta.originY != 0.0f || delta.scale != 0.0f;
            }

            CDeformerEditContext context;
            QVERIFY2(context.Build(document, target, type == 0 ? "warp" : "rotation", 0, pose, &error),
                     qPrintable(target + ": " + error));
            C2DTree tree(table);
            tree.SetPoseOverrides(overrides);
            for (auto* node : tree.getParametersTree())
                if (auto* parameter = dynamic_cast<CParameterNode*>(node)) parameter->SetCurValue(pose.value(parameter->getName()));
            const auto nodes = tree.GetWarpKeys();
            const auto model = [](QPointF point) { return QPointF(point.x(), -point.y()); };
            const C2DTreeNode* targetNode = nullptr;
            for (const auto* node : nodes) if (node && node->getName() == target) { targetNode = node; break; }
            QVERIFY(targetNode);
            if (type == 0) {
                const auto local = document.DeformerWarpKeyforms(target).at(0).positions;
                QVector<float> world, restored;
                QVERIFY2(context.ToModel(local, world, &error), qPrintable(error));
                QVERIFY2(context.FromModel(world, restored, &error), qPrintable(error));
                QVERIFY(near(restored, local));
                QVector<float> expected;
                for (const float value : targetNode->GetCanvasPositions()) expected.append(value);
                for (qsizetype index = 1; index < expected.size(); index += 2) expected[index] = -expected[index];
                QVERIFY(near(context.ModelPoints(), expected));
            } else {
                const auto reference = document.DeformerRotationKeyforms(target).at(0);
                CRotationFrameGeometry::Frame frame, desired;
                QVERIFY2(context.CurrentRotationFrame(frame, &error), qPrintable(error));
                const auto* rotation = dynamic_cast<const CRotationNode*>(targetNode);
                QVERIFY(rotation && rotation->GetCanvasBasis());
                const auto& basis = *rotation->GetCanvasBasis();
                QVERIFY(std::hypot(frame.origin.x() - model(basis.origin).x(), frame.origin.y() - model(basis.origin).y()) < 0.00001);
                QVERIFY(std::hypot(frame.xAxis.x() - model(basis.xAxis).x(), frame.xAxis.y() - model(basis.xAxis).y()) < 0.00001);
                QVERIFY(std::hypot(frame.yAxis.x() - model(basis.yAxis).x(), frame.yAxis.y() - model(basis.yAxis).y()) < 0.00001);
                desired = frame; desired.origin += QPointF(0.031, -0.017);
                CModelRotationKeyform solved;
                QVERIFY2(context.SolveRotation(desired, reference, solved, &error,
                    CDeformerEditContext::ReflectionPolicy::Preserve), qPrintable(error));
                CRotationFrameGeometry::Frame rebuilt;
                QVERIFY2(context.ResolveRotation(solved, rebuilt, &error), qPrintable(error));
                QVERIFY2(CRotationFrameGeometry::Matches(rebuilt, desired, &error), qPrintable(error));
            }
        }
        QVERIFY(rotationDeltaObserved);
    }
};
QTEST_GUILESS_MAIN(DeformerEditContextCoreTests)
#include "deformer_edit_context_core_tests.moc"
