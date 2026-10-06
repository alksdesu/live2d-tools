#include "deformer_edit_context_fixture.h"
#include "live2dstruct/cblendshapeposeevaluator.h"
#include "live2dstruct/tree/c2dtree.h"
#include "live2dstruct/tree/crotationnode.h"

class DeformerEditContextCoreTests : public QObject
{
    Q_OBJECT
private slots:
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
