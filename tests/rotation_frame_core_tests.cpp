#include "deformer_edit_context_fixture.h"
#include "live2dstruct/crotationframegeometry.h"
#include "live2dstruct/keyform/clinearinterpolation.h"
#include "live2dstruct/tree/crotationnode.h"
#include "live2dstruct/tree/cparameternode.h"

class RotationFrameCoreTests : public QObject
{
    Q_OBJECT
private slots:
    void reflectedFrameRebuildsCoreWitness_data()
    {
        QTest::addColumn<QString>("parent");
        QTest::addColumn<float>("scale");
        QTest::addColumn<int>("reflection");
        for (const QString parent : {QString("root"), QString("rotation"), QString("warp"), QString("nested")})
            for (float scale : {-0.5f, -0.0f, 0.0f, 0.5f}) for (int reflection = 0; reflection < 4; ++reflection)
                QTest::newRow(qPrintable(parent + '-' + (scale == 0 && std::signbit(scale) ? QString("negative-zero") :
                    QString::number(scale)) + '-' + QString::number(reflection)))
                    << parent << scale << reflection;
    }

    void reflectedFrameRebuildsCoreWitness()
    {
        QFETCH(QString, parent); QFETCH(float, scale); QFETCH(int, reflection);
        const auto source = fixture("rotation", parent, scale, reflection);
        QVERIFY(!source.bytes.isEmpty());
        CModelDocument document;
        QString error;
        QVERIFY2(document.LoadBytes(source.bytes, "reflection.moc3", &error), qPrintable(error));
        const auto pose = targetPose(document, source);
        CDeformerEditContext context;
        QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        CRotationFrameGeometry::Frame original, mirrored;
        QVERIFY2(context.CurrentRotationFrame(original, &error), qPrintable(error));
        CoreModel baseline;
        QVERIFY(baseline.load(source.bytes)); setPose(baseline, pose);
        const auto originalPoints = witnessPoints(baseline, source, 3);
        QVERIFY(near(originalPoints.mid(0, 2), {float(original.origin.x()), float(original.origin.y())}));
        const QPointF line(original.origin.x() + 0.015, original.origin.y());
        QVERIFY(CRotationFrameGeometry::Mirror(original, line, {1, 0}, mirrored, &error));
        auto forms = document.DeformerRotationKeyforms(source.id);
        auto reference = forms[0]; reference.angle += 720;
        CModelRotationKeyform solved = reference;
        QVERIFY(!context.SolveRotation(mirrored, reference, solved, &error));
        QVERIFY(error.contains(QStringLiteral("反射")));
        QCOMPARE(solved, reference);
        QVERIFY2(context.SolveRotation(mirrored, reference, solved, &error,
            CDeformerEditContext::ReflectionPolicy::AllowChange), qPrintable(error));
        QVERIFY(std::abs(double(solved.angle) - reference.angle) <= 180.0001);
        QCOMPARE(solved.reflectX, reference.reflectX);
        QCOMPARE(solved.reflectY, !reference.reflectY);
        QCOMPARE(solved.opacity, reference.opacity);
        QCOMPARE(std::signbit(solved.scale), std::signbit(reference.scale));
        forms[0] = solved;
        QVERIFY(document.SetDeformerRotationKeyforms(source.id, forms));
        QByteArray candidate;
        QVERIFY2(document.BuildPreviewMoc(candidate, &error), qPrintable(error));
        CoreModel rebuilt;
        QVERIFY(rebuilt.load(candidate)); setPose(rebuilt, pose);
        const auto actual = witnessPoints(rebuilt, source, 3);
        QVector<float> expected = originalPoints;
        for (int vertex = 0; vertex < expected.size() / 2; ++vertex)
            expected[vertex * 2] = float(2 * line.x() - expected[vertex * 2]);
        QVERIFY2(near(actual, expected), qPrintable(parent + " " + error));
        QCOMPARE(frame(baseline.value).definition, frame(rebuilt.value).definition);
        QVERIFY(baseline.intact()); QVERIFY(rebuilt.intact());
    }

    void movedOriginResolvesNewNonlinearParentDirection()
    {
        const auto source = fixture("rotation", "warp", -0.5f, 1);
        CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(source.bytes, "nonlinear.moc3"));
        const auto pose = targetPose(document, source);
        CDeformerEditContext context;
        QVERIFY2(context.Build(document, source.id, source.type, 0, pose, &error), qPrintable(error));
        auto reference = document.DeformerRotationKeyforms(source.id)[0];
        auto moved = reference; moved.originX = 0.72f; moved.originY = 0.61f;
        CRotationFrameGeometry::Frame original, desired;
        QVERIFY(context.ResolveRotation(reference, original, &error));
        QVERIFY(context.ResolveRotation(moved, desired, &error));
        QVERIFY(std::hypot(desired.xAxis.x() - original.xAxis.x(), desired.xAxis.y() - original.xAxis.y()) > 0.001);
        CModelRotationKeyform solved;
        QVERIFY2(context.SolveRotation(desired, reference, solved, &error), qPrintable(error));
        QVERIFY(std::abs(solved.angle - moved.angle) < 0.0001);
        QVERIFY(std::abs(solved.originX - moved.originX) < 0.00001);
        QVERIFY(std::abs(solved.originY - moved.originY) < 0.00001);
        auto forms = document.DeformerRotationKeyforms(source.id); forms[0] = solved;
        QVERIFY(document.SetDeformerRotationKeyforms(source.id, forms));
        QByteArray bytes; QVERIFY(document.BuildPreviewMoc(bytes, &error));
        CoreModel core; QVERIFY(core.load(bytes)); setPose(core, pose);
        const auto actual = witnessPoints(core, source, 3);
        for (int vertex = 0; vertex < 3; ++vertex) {
            const QPointF expected = desired.origin + (vertex == 1 ? desired.xAxis * desired.scale :
                vertex == 2 ? desired.yAxis * desired.scale : QPointF());
            QVERIFY(std::hypot(actual[vertex * 2] - expected.x(), actual[vertex * 2 + 1] - expected.y()) < 0.00002);
        }
        QVERIFY(core.intact());
    }

    void reflectedFramesTransferBetweenDifferentParents_data()
    {
        QTest::addColumn<QString>("sourceParent"); QTest::addColumn<QString>("targetParent");
        QTest::addColumn<float>("scale");
        for (const auto& parents : {qMakePair(QString("root"), QString("nested")),
                                   qMakePair(QString("nested"), QString("root")),
                                   qMakePair(QString("warp"), QString("rotation"))})
            for (float scale : {-0.5f, 0.0f, 0.5f})
                QTest::newRow(qPrintable(parents.first + '-' + parents.second + '-' + QString::number(scale)))
                    << parents.first << parents.second << scale;
    }

    void reflectedFramesTransferBetweenDifferentParents()
    {
        QFETCH(QString, sourceParent); QFETCH(QString, targetParent); QFETCH(float, scale);
        const auto source = fixture("rotation", sourceParent, scale, 1);
        const auto target = fixture("rotation", targetParent, -0.25f, 3);
        CModelDocument sourceDocument, targetDocument; QString error;
        QVERIFY(sourceDocument.LoadBytes(source.bytes, "source.moc3"));
        QVERIFY(targetDocument.LoadBytes(target.bytes, "target.moc3"));
        const auto sourcePose = targetPose(sourceDocument, source), targetValues = targetPose(targetDocument, target);
        CDeformerEditContext sourceContext, targetContext;
        QVERIFY(sourceContext.Build(sourceDocument, source.id, source.type, 0, sourcePose, &error));
        QVERIFY(targetContext.Build(targetDocument, target.id, target.type, 0, targetValues, &error));
        CRotationFrameGeometry::Frame sourceFrame, targetFrame, desired;
        QVERIFY(sourceContext.CurrentRotationFrame(sourceFrame));
        QVERIFY(targetContext.CurrentRotationFrame(targetFrame));
        QPointF normal = sourceFrame.origin - targetFrame.origin;
        if (normal.isNull()) normal = {1, 0};
        const QPointF center = (sourceFrame.origin + targetFrame.origin) / 2;
        QVERIFY(CRotationFrameGeometry::Mirror(sourceFrame, center, normal, desired));
        auto forms = targetDocument.DeformerRotationKeyforms(target.id);
        CModelRotationKeyform solved;
        QVERIFY2(targetContext.SolveRotation(desired, forms[0], solved, &error,
            CDeformerEditContext::ReflectionPolicy::AllowChange), qPrintable(error));
        forms[0] = solved;
        QVERIFY(targetDocument.SetDeformerRotationKeyforms(target.id, forms));
        QByteArray candidate; QVERIFY(targetDocument.BuildPreviewMoc(candidate, &error));
        CoreModel sourceCore, targetCore;
        QVERIFY(sourceCore.load(source.bytes)); setPose(sourceCore, sourcePose);
        QVERIFY(targetCore.load(candidate)); setPose(targetCore, targetValues);
        const auto before = witnessPoints(sourceCore, source, 3), actual = witnessPoints(targetCore, target, 3);
        normal /= std::hypot(normal.x(), normal.y());
        for (int vertex = 0; vertex < 3; ++vertex) {
            const QPointF original(before[vertex * 2], before[vertex * 2 + 1]);
            const QPointF expected = original - 2 * QPointF::dotProduct(original - center, normal) * normal;
            QVERIFY(std::hypot(expected.x() - actual[vertex * 2], expected.y() - actual[vertex * 2 + 1]) < 0.00002);
        }
        QVERIFY(sourceCore.intact()); QVERIFY(targetCore.intact());
    }

    void foldedParentAndNonRepresentableFramesPreserveOutputs()
    {
        const auto source = fixture("rotation", "warp", 0.5f, 0);
        CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(source.bytes, "atomic.moc3"));
        CDeformerEditContext context;
        QVERIFY(context.Build(document, source.id, source.type, 0, targetPose(document, source), &error));
        const auto reference = document.DeformerRotationKeyforms(source.id)[0];
        CRotationFrameGeometry::Frame desired;
        QVERIFY(context.CurrentRotationFrame(desired));
        auto invalid = desired; invalid.scale = std::numeric_limits<double>::denorm_min();
        CModelRotationKeyform output = reference;
        QVERIFY(!context.SolveRotation(invalid, reference, output, &error));
        QCOMPARE(output, reference);
        invalid = desired; invalid.xAxis = invalid.yAxis;
        QVERIFY(!context.SolveRotation(invalid, reference, output, &error));
        QCOMPARE(output, reference);
        const auto folded = fixture("rotation", "folded", 0.5f, 0);
        CModelDocument foldedDocument;
        QVERIFY(foldedDocument.LoadBytes(folded.bytes, "folded.moc3"));
        CDeformerEditContext foldedContext;
        QVERIFY(foldedContext.Build(foldedDocument, folded.id, folded.type, 0, targetPose(foldedDocument, folded), &error));
        const auto foldedReference = foldedDocument.DeformerRotationKeyforms(folded.id)[0];
        QVERIFY(foldedContext.CurrentRotationFrame(desired));
        desired.origin = {1, -1};
        output = reference;
        QVERIFY(!foldedContext.SolveRotation(desired, foldedReference, output, &error));
        QCOMPARE(output, reference);
        QVERIFY(!error.isEmpty());
    }

    void unchangedFrameRetainsExactAuthoringValues()
    {
        const auto source = fixture("rotation", "nested", -0.5f, 3);
        CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(source.bytes, "unchanged.moc3"));
        CDeformerEditContext context;
        QVERIFY(context.Build(document, source.id, source.type, 0, targetPose(document, source), &error));
        auto reference = document.DeformerRotationKeyforms(source.id)[0]; reference.angle += 720;
        CRotationFrameGeometry::Frame desired;
        QVERIFY(context.ResolveRotation(reference, desired, &error));
        CModelRotationKeyform actual;
        QVERIFY2(context.SolveRotation(desired, reference, actual, &error), qPrintable(error));
        QCOMPARE(actual, reference);
    }

    void exactNoopDetectionDoesNotUseFuzzyPointEquality()
    {
        const auto source = fixture("rotation", "root", 0.5f, 0);
        CModelDocument document; QString error;
        QVERIFY(document.LoadBytes(source.bytes, "tiny-angle.moc3"));
        CDeformerEditContext context;
        QVERIFY(context.Build(document, source.id, source.type, 0, targetPose(document, source), &error));
        auto reference = document.DeformerRotationKeyforms(source.id)[0]; reference.angle = 0;
        auto changed = reference; changed.angle = 0.00000000001f;
        CRotationFrameGeometry::Frame desired;
        QVERIFY(context.ResolveRotation(changed, desired, &error));
        CModelRotationKeyform actual;
        QVERIFY2(context.SolveRotation(desired, reference, actual, &error), qPrintable(error));
        QVERIFY(actual.angle > reference.angle);
    }
};

QTEST_GUILESS_MAIN(RotationFrameCoreTests)
#include "rotation_frame_core_tests.moc"
