#include "deformer_edit_context_fixture.h"

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
};
QTEST_GUILESS_MAIN(DeformerEditContextCoreTests)
#include "deformer_edit_context_core_tests.moc"
